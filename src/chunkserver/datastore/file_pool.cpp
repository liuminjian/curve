/*
 *  Copyright (c) 2020 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Project: curve
 * File Created: Monday, 10th December 2018 9:54:45 am
 * Author: tongguangxun
 */

#include "src/chunkserver/datastore/file_pool.h"

#include <brpc/reloadable_flags.h>
#include <errno.h>
#include <fcntl.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <json/json.h>
#include <linux/fs.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "absl/utility/utility.h"
#include "src/common/configuration.h"
#include "src/common/crc32.h"
#include "src/common/curve_define.h"
#include "src/common/string_util.h"
#include "src/common/throttle.h"

using curve::common::kFilePoolMagic;
DEFINE_int64(formatInterval, 100, "Sets a interval between formatting.");
DEFINE_validator(formatInterval, brpc::PositiveInteger);

namespace curve {
namespace chunkserver {
const char* FilePoolHelper::kFileSize = "chunkSize";
const char* FilePoolHelper::kMetaPageSize = "metaPageSize";
const char* FilePoolHelper::kFilePoolPath = "chunkfilepool_path";
const char* FilePoolHelper::kCRC = "crc";
const char* FilePoolHelper::kBlockSize = "blockSize";
const uint32_t FilePoolHelper::kPersistSize = 4096;
const std::string FilePool::kCleanChunkSuffix_ = ".clean";  // NOLINT
const std::chrono::milliseconds FilePool::kSuccessSleepMsec_(10);
const std::chrono::milliseconds FilePool::kFailSleepMsec_(500);

using ::curve::common::kDefaultBlockSize;

namespace {

std::ostream& operator<<(std::ostream& os, const FilePoolMeta& meta) {
    os << "chunksize: " << meta.chunkSize
       << ", metapagesize: " << meta.metaPageSize
       << ", hasblocksize: " << meta.hasBlockSize
       << ", blocksize: " << meta.blockSize
       << ", filepoolpath: " << meta.filePoolPath;
    return os;
}

}  // namespace

int FilePoolHelper::PersistEnCodeMetaInfo(
    std::shared_ptr<LocalFileSystem> fsptr,
    const FilePoolMeta& meta,
    const std::string& persistPath) {
    Json::Value root;
    root[kFileSize] = meta.chunkSize;
    root[kMetaPageSize] = meta.metaPageSize;
    if (meta.hasBlockSize) {
        root[kBlockSize] = meta.blockSize;
    }
    root[kFilePoolPath] = meta.filePoolPath;
    root[kCRC] = meta.Crc32();

    int fd = fsptr->Open(persistPath.c_str(), O_RDWR | O_CREAT | O_SYNC);
    if (fd < 0) {
        LOG(ERROR) << "meta file open failed, " << persistPath.c_str();
        return -1;
    }

    LOG(INFO) << root.toStyledString();

    char *writeBuffer = new char[kPersistSize];
    memset(writeBuffer, 0, kPersistSize);
    memcpy(writeBuffer, root.toStyledString().c_str(),
           root.toStyledString().size());

    int ret = fsptr->Write(fd, writeBuffer, 0, kPersistSize);
    if (ret != kPersistSize) {
        LOG(ERROR) << "meta file write failed, " << persistPath.c_str()
                   << ", ret = " << ret;
        fsptr->Close(fd);
        delete[] writeBuffer;
        return -1;
    }

    fsptr->Close(fd);
    delete[] writeBuffer;
    return 0;
}

int FilePoolHelper::DecodeMetaInfoFromMetaFile(
    std::shared_ptr<LocalFileSystem> fsptr,
    const std::string& metaFilePath,
    uint32_t metaFileSize,
    FilePoolMeta* meta) {
    int fd = fsptr->Open(metaFilePath, O_RDONLY);
    if (fd < 0) {
        LOG(ERROR) << "meta file open failed, " << metaFilePath;
        return -1;
    }

    std::unique_ptr<char[]> readvalid(new char[metaFileSize]);
    memset(readvalid.get(), 0, metaFileSize);
    int ret = fsptr->Read(fd, readvalid.get(), 0, metaFileSize);
    if (ret != static_cast<int>(metaFileSize)) {
        fsptr->Close(fd);
        LOG(ERROR) << "meta file read failed, " << metaFilePath;
        return -1;
    }

    fsptr->Close(fd);

    uint32_t crcvalue = 0;
    bool parse = false;
    do {
        Json::CharReaderBuilder builder;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        Json::Value value;
        char *str = readvalid.get();
        JSONCPP_STRING errormsg;
        if (!reader->parse(str, str + strlen(str), &value, &errormsg)) {
            LOG(ERROR) << "chunkfile meta file got error!"
                       << " error: " << errormsg;
            break;
        }

        if (!value[kFileSize].isNull()) {
            meta->chunkSize = value[kFileSize].asUInt();
        } else {
            LOG(ERROR) << "chunkfile meta file got error!"
                       << " no chunksize!";
            break;
        }

        if (!value[kMetaPageSize].isNull()) {
            meta->metaPageSize = value[kMetaPageSize].asUInt();
        } else {
            LOG(ERROR) << "chunkfile meta file got error!"
                       << " no metaPageSize!";
            break;
        }

        if (!value[kBlockSize].isNull()) {
            meta->hasBlockSize = true;
            meta->blockSize = value[kBlockSize].asUInt();
        } else {
            meta->hasBlockSize = false;
            meta->blockSize = kDefaultBlockSize;
            LOG(WARNING) << "chunkfile meta file doesn't has `" << kBlockSize
                         << "`, use default value " << kDefaultBlockSize;
        }

        if (!value[kFilePoolPath].isNull()) {
            meta->filePoolPath = value[kFilePoolPath].asString();
        } else {
            LOG(ERROR) << "chunkfile meta file got error!"
                       << " no FilePool path!";
            break;
        }

        if (!value[kCRC].isNull()) {
            crcvalue = value[kCRC].asUInt();
        } else {
            LOG(ERROR) << "chunkfile meta file got error!"
                       << " no crc!";
            break;
        }

        parse = true;
    } while (false);

    if (!parse) {
        LOG(ERROR) << "parse meta file failed! " << metaFilePath;
        return -1;
    }

    auto crcCalc = meta->Crc32();
    if (crcvalue != crcCalc) {
        LOG(ERROR) << "crc check failed, calculate crc: " << crcCalc
                   << ", record: " << crcvalue << ", decoded meta: " << *meta;
        return -1;
    }

    return 0;
}

FilePool::FilePool(std::shared_ptr<LocalFileSystem> fsptr)
    : currentmaxfilenum_(0) {
    CHECK(fsptr != nullptr) << "fs ptr allocate failed!";
    fsptr_ = fsptr;
    cleanAlived_ = false;

    writeBuffer_.reset(new char[poolOpt_.bytesPerWrite]);
    memset(writeBuffer_.get(), 0, poolOpt_.bytesPerWrite);
}

FilePool::~FilePool() { UnInitialize(); }

bool FilePool::Initialize(const FilePoolOptions &cfopt) {
    poolOpt_ = cfopt;
    if (poolOpt_.getFileFromPool) {
        currentdir_ = poolOpt_.filePoolDir;
        currentState_.chunkSize = poolOpt_.fileSize;
        currentState_.metaPageSize = poolOpt_.metaPageSize;
        if (!CheckValid()) {
            LOG(ERROR) << "Check vaild failed!";
            return false;
        }

        if (!ScanInternal()) {
            LOG(ERROR) << "Scan pool files failed!";
            return false;
        }

        if (!PrepareFormat()) {
            LOG(ERROR) << "Prepare format failed!";
            return false;
        }

        formatAlived_.store(true);
        formatThread_ = Thread(&FilePool::FormatWorker, this);
    } else {
        currentdir_ = poolOpt_.filePoolDir;
        if (!fsptr_->DirExists(currentdir_.c_str())) {
            return fsptr_->Mkdir(currentdir_.c_str()) == 0;
        }
    }
    return true;
}

bool FilePool::CheckValid() {
    FilePoolMeta meta;
    if (!fsptr_->FileExists(poolOpt_.metaPath)) {
        LOG(INFO) << "Metafile in path '" << poolOpt_.metaPath
                  << "'not found, it's the first initialized.";
        currentState_.chunkSize = poolOpt_.fileSize;
        currentState_.metaPageSize = poolOpt_.metaPageSize;
        currentState_.blockSize = poolOpt_.blockSize;
        currentdir_ = poolOpt_.filePoolDir;
        return true;
    }
    int ret = FilePoolHelper::DecodeMetaInfoFromMetaFile(
        fsptr_, poolOpt_.metaPath, poolOpt_.metaFileSize, &meta);
    if (ret == -1) {
        LOG(ERROR) << "Decode meta info from meta file failed!";
        return false;
    }

    // reset options from meta file
    if (poolOpt_.fileSize != meta.chunkSize) {
        auto old = absl::exchange(poolOpt_.fileSize, meta.chunkSize);
        LOG(WARNING) << "Reset file size from " << old << " to "
                     << poolOpt_.fileSize;
    }
    if (poolOpt_.metaPageSize != meta.metaPageSize) {
        auto old = absl::exchange(poolOpt_.metaPageSize, meta.metaPageSize);
        LOG(WARNING) << "Reset meta page size from " << old << " to "
                     << poolOpt_.metaFileSize;
    }
    if (poolOpt_.blockSize != meta.blockSize) {
        auto old = absl::exchange(poolOpt_.blockSize, meta.blockSize);
        LOG(WARNING) << "Reset block size from " << old << " to "
                     << poolOpt_.blockSize;
    }

    currentdir_ = std::move(meta.filePoolPath);
    currentState_.chunkSize = meta.chunkSize;
    currentState_.metaPageSize = meta.metaPageSize;
    currentState_.blockSize = meta.blockSize;
    return true;
}

int FilePool::CleanChunk(uint64_t chunkid, bool onlyMarked) {
    std::string chunkpath = currentdir_ + "/" + std::to_string(chunkid);
    int ret = fsptr_->Open(chunkpath, O_RDWR);
    if (ret < 0) {
        LOG(ERROR) << "Open file failed: " << chunkpath;
        return ret;
    }

    int fd = ret;
    auto defer = [&](...) { fsptr_->Close(fd); };
    std::shared_ptr<void> _(nullptr, defer);

    uint64_t chunklen = poolOpt_.fileSize + poolOpt_.metaPageSize;
    if (onlyMarked) {
        ret = fsptr_->Fallocate(fd, FALLOC_FL_ZERO_RANGE, 0, chunklen);
        if (ret < 0) {
            LOG(ERROR) << "Fallocate file failed: " << chunkpath;
            return ret;
        }
    } else {
        int nbytes;
        uint64_t nwrite = 0;
        uint64_t ntotal = chunklen;
        uint32_t bytesPerWrite = poolOpt_.bytesPerWrite;
        char *buffer = writeBuffer_.get();

        while (nwrite < ntotal) {
            nbytes = fsptr_->Write(
                fd, buffer, nwrite,
                std::min(ntotal - nwrite, (uint64_t)bytesPerWrite));
            if (nbytes < 0) {
                LOG(ERROR) << "Write file failed: " << chunkpath;
                return nbytes;
            } else if (fsptr_->Fsync(fd) < 0) {
                LOG(ERROR) << "Fsync file failed: " << chunkpath;
                return nbytes;
            }

            cleanThrottle_.Add(false, bytesPerWrite);
            nwrite += nbytes;
        }
    }

    std::string targetpath = chunkpath + kCleanChunkSuffix_;
    ret = fsptr_->Rename(chunkpath, targetpath);
    if (ret < 0) {
        LOG(ERROR) << "Rename file failed: " << chunkpath;
        return ret;
    }

    return ret;
}

bool FilePool::CleaningChunk() {
    auto popBack = [this](std::vector<uint64_t> *chunks,
                          uint64_t *chunksLeft) -> uint64_t {
        std::unique_lock<std::mutex> lk(mtx_);
        if (chunks->empty()) {
            return 0;
        }

        uint64_t chunkid = chunks->back();
        chunks->pop_back();
        (*chunksLeft)--;
        currentState_.preallocatedChunksLeft--;
        return chunkid;
    };

    auto pushBack = [this](std::vector<uint64_t> *chunks, uint64_t chunkid,
                           uint64_t *chunksLeft) {
        std::unique_lock<std::mutex> lk(mtx_);
        chunks->push_back(chunkid);
        (*chunksLeft)++;
        currentState_.preallocatedChunksLeft++;
    };

    uint64_t chunkid = popBack(&dirtyChunks_, &currentState_.dirtyChunksLeft);
    if (0 == chunkid) {
        return false;
    }

    // Fill zero to specify chunk
    int ret = CleanChunk(chunkid, false);
    if (ret < 0) {
        pushBack(&dirtyChunks_, chunkid, &currentState_.dirtyChunksLeft);
        return false;
    }

    LOG(INFO) << "Clean chunk success, chunkid: " << chunkid;
    pushBack(&cleanChunks_, chunkid, &currentState_.cleanChunksLeft);
    return true;
}

void FilePool::CleanWorker() {
    auto sleepInterval = kSuccessSleepMsec_;
    while (cleanSleeper_.wait_for(sleepInterval)) {
        sleepInterval = CleaningChunk() ? kSuccessSleepMsec_ : kFailSleepMsec_;
    }
}

bool FilePool::PrepareFormat() {
    curve::fs::FileSystemInfo finfo;
    int r = fsptr_->Statfs(currentdir_, &finfo);
    if (r != 0) {
        LOG(ERROR) << "get disk usage info failed!";
        return false;
    }

    if (poolOpt_.allocatedByPercent) {
        poolOpt_.filePoolSize = finfo.total * poolOpt_.allocatedPercent / 100;
    }

    uint64_t bytesPerPage = poolOpt_.fileSize + poolOpt_.metaFileSize;
    uint64_t needSpace =
        poolOpt_.filePoolSize - currentState_.chunkNum * bytesPerPage;
    uint64_t vaildSpace = 0;

    if (poolOpt_.filePoolSize / bytesPerPage < currentState_.chunkNum) {
        LOG(INFO) << "It is no need to format chunks.";
        formatStat_.preAllocateNum = 0;
        formatStat_.allocateChunkNum = 0;
        return true;
    }

    vaildSpace = finfo.available;
    LOG(INFO) << "free space = " << finfo.available
              << ", total space = " << finfo.total
              << ", need space = " << needSpace;
    if (vaildSpace < needSpace) {
        LOG(ERROR) << "disk free space not enough.";
        return false;
    }

    formatStat_.preAllocateNum = needSpace / bytesPerPage;
    formatStat_.allocateChunkNum = 0;
    LOG(INFO) << "preAllocateNum = " << formatStat_.preAllocateNum;
    return true;
}

bool FilePool::WaitoFormatDoneForTesting() {
    std::unique_lock<std::mutex> lk(mtx_);
    if (formatStat_.allocateChunkNum.load() != formatStat_.preAllocateNum) {
        cond_.wait(lk, [&]() {
            return formatStat_.allocateChunkNum.load() ==
                   formatStat_.preAllocateNum;
        });
    }
    lk.unlock();
    if (formatThread_.joinable()) {
        formatThread_.join();
    }
    return true;
}

bool FilePool::StopFormatting() {
    if (formatAlived_.exchange(false)) {
        LOG(INFO) << "Stop formatting...";
        if (formatThread_.joinable()) {
            formatThread_.join();
        }
        LOG(INFO) << "Stop format thread ok.";
    }
    return true;
}
int FilePool::FormatTask(uint64_t indexOffset,
                         std::atomic<uint32_t>* allocatIndex) {
    LOG(INFO) << "format thread has been work!";
    while (!this->formatStat_.isWrong.load() && this->formatAlived_.load()) {
        uint32_t chunkIndex = 0;
        if ((chunkIndex = allocatIndex->fetch_add(1)) >=
            formatStat_.preAllocateNum) {
            allocatIndex->fetch_sub(1);
            break;
        }
        std::string chunkPath = this->currentdir_ + "/" +
                                std::to_string(chunkIndex + indexOffset) +
                                kCleanChunkSuffix_;
        this->formatSleeper_.wait_for(
            std::chrono::milliseconds{FLAGS_formatInterval});
        int res = this->AllocateChunk(chunkPath);
        if (res != 0) {
            this->formatStat_.isWrong.store(true);
            LOG(ERROR) << "Format ERROR!";
            break;
        }
        this->mtx_.lock();
        cleanChunks_.push_back(chunkIndex + indexOffset);
        this->currentState_.cleanChunksLeft++;
        this->currentState_.preallocatedChunksLeft++;
        this->currentState_.chunkNum++;
        this->formatStat_.allocateChunkNum++;
        this->mtx_.unlock();
        this->cond_.notify_all();
    }
    LOG(INFO) << "format thread has done!";
    return 0;
}

int FilePool::FormatWorker() {
    std::vector<Thread> threads;
    uint64_t offset =
        this->currentmaxfilenum_.fetch_add(formatStat_.preAllocateNum);
    std::atomic<uint32_t> allocatIndex{0};

    for (uint32_t i = 0; i < poolOpt_.formatThreadNum; i++) {
        threads.emplace_back(
            Thread(&FilePool::FormatTask, this, offset, &allocatIndex));
    }

    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    LOG(INFO) << "format worker done";

    if (formatStat_.isWrong.load()) {
        LOG(ERROR) << "Chunk format failed!";
        return -1;
    }
    return 0;
}

bool FilePool::StartCleaning() {
    if (poolOpt_.needClean && !cleanAlived_.exchange(true)) {
        ReadWriteThrottleParams params;
        params.iopsTotal = ThrottleParams(poolOpt_.iops4clean, 0, 0);
        cleanThrottle_.UpdateThrottleParams(params);

        cleanThread_ = Thread(&FilePool::CleanWorker, this);
        LOG(INFO) << "Start clean thread ok.";
    }

    return true;
}

bool FilePool::StopCleaning() {
    if (cleanAlived_.exchange(false)) {
        LOG(INFO) << "Stop cleaning...";
        cleanSleeper_.interrupt();
        cleanThread_.join();
        LOG(INFO) << "Stop clean thread ok.";
    }

    return true;
}

int FilePool::GetChunk(bool needClean, uint64_t *chunkid, bool *isCleaned) {
    auto pop = [&](std::vector<uint64_t>* chunks, uint64_t* chunksLeft,
                   bool isCleanChunks) -> bool {
        if (chunks->empty()) {
            return false;
        }

        *chunkid = chunks->back();
        chunks->pop_back();
        (*chunksLeft)--;
        currentState_.preallocatedChunksLeft--;
        *isCleaned = isCleanChunks;
        return true;
    };

    auto wake_up = [&]() {
        return (formatStat_.allocateChunkNum.load() ==
                formatStat_.preAllocateNum)  // NOLINT
               || !(dirtyChunks_.empty() && cleanChunks_.empty());
    };

    bool ret = true;
    {
        std::unique_lock<std::mutex> lk(mtx_);
        if (formatStat_.allocateChunkNum.load() != formatStat_.preAllocateNum) {
            cond_.wait(lk, wake_up);
        }
        if (!needClean) {
            ret =  pop(&dirtyChunks_, &currentState_.dirtyChunksLeft, false) ||
                   pop(&cleanChunks_, &currentState_.cleanChunksLeft, true);
            return ret ? 0 : -1;
        }

        // Need clean chunk
        *isCleaned = false;
        ret = pop(&cleanChunks_, &currentState_.cleanChunksLeft, true) ||
              pop(&dirtyChunks_, &currentState_.dirtyChunksLeft, false);
    }
    int cleanRet = -1;
    if (true == ret && false == *isCleaned) {
        cleanRet = CleanChunk(*chunkid, true);
        if (cleanRet < 0) {
            *isCleaned = false;
        } else {
            *isCleaned = true;
        }
    }

    return *isCleaned ? 0 : cleanRet;
}

int FilePool::GetFile(const std::string &targetpath, const char *metapage,
                      bool needClean) {
    int ret = -1;
    int retry = 0;

    while (retry < poolOpt_.retryTimes) {
        uint64_t chunkID;
        std::string srcpath;
        if (poolOpt_.getFileFromPool) {
            bool isCleaned = false;
            ret = GetChunk(needClean, &chunkID, &isCleaned);
            if (ret < 0) {
                LOG(ERROR) << "No avaliable chunk!";
                break;
            }
            srcpath = currentdir_ + "/" + std::to_string(chunkID);
            if (isCleaned) {
                srcpath = srcpath + kCleanChunkSuffix_;
            }
        } else {
            srcpath = currentdir_ + "/" +
                      std::to_string(currentmaxfilenum_.fetch_add(1));
            ret = AllocateChunk(srcpath);
            if (ret < 0) {
                LOG(ERROR) << "file allocate failed, " << srcpath.c_str();
                retry++;
                continue;
            }
        }

        ret = WriteMetaPage(srcpath, metapage);
        if (ret >= 0) {
            // Here, the RENAME_NOREPLACE mode is used to rename the file.
            // When the target file exists, it is not allowed to be overwritten.
            // That is to say, creating a file through FilePool needs to ensure
            // that the target file does not exist. Datastore may have scenarios
            // where files are created concurrently. Rename is used to ensure
            // the atomicity of file creation, and to ensure that existing files
            // will not be overwritten.
            ret = fsptr_->Rename(srcpath.c_str(), targetpath.c_str(),
                                 RENAME_NOREPLACE);
            // The target file already exists, exit the current logic directly,
            // and delete the written file
            if (ret == -EEXIST) {
                LOG(ERROR) << targetpath
                           << ", already exists! src path = " << srcpath;
                break;
            } else if (ret < 0) {
                LOG(ERROR) << "file rename failed, " << srcpath.c_str();
            } else {
                LOG(INFO) << "get file " << targetpath
                          << " success! now pool size = "
                          << currentState_.preallocatedChunksLeft;
                break;
            }
        } else {
            LOG(ERROR) << "write metapage failed, " << srcpath.c_str();
        }
        retry++;
    }
    return ret;
}

int FilePool::AllocateChunk(const std::string &chunkpath) {
    uint64_t chunklen = poolOpt_.fileSize + poolOpt_.metaPageSize;

    int ret = fsptr_->Open(chunkpath.c_str(), O_RDWR | O_CREAT);
    if (ret < 0) {
        LOG(ERROR) << "file open failed, " << chunkpath.c_str();
        return -1;
    }
    int fd = ret;

    ret = fsptr_->Fallocate(fd, 0, 0, chunklen);
    if (ret < 0) {
        fsptr_->Close(fd);
        LOG(ERROR) << "Fallocate failed, " << chunkpath.c_str();
        return ret;
    }

    char *data = new (std::nothrow) char[chunklen];
    memset(data, 0, chunklen);

    ret = fsptr_->Write(fd, data, 0, chunklen);
    if (ret < 0) {
        fsptr_->Close(fd);
        delete[] data;
        LOG(ERROR) << "write failed, " << chunkpath.c_str();
        return ret;
    }
    delete[] data;

    ret = fsptr_->Fsync(fd);
    if (ret < 0) {
        fsptr_->Close(fd);
        LOG(ERROR) << "fsync failed, " << chunkpath.c_str();
        return ret;
    }

    ret = fsptr_->Close(fd);
    if (ret != 0) {
        LOG(ERROR) << "close failed, " << chunkpath.c_str();
    }
    return ret;
}

int FilePool::WriteMetaPage(const std::string &sourcepath, const char *page) {
    int fd = -1;
    int ret = -1;

    ret = fsptr_->Open(sourcepath.c_str(), O_RDWR);
    if (ret < 0) {
        LOG(ERROR) << "file open failed, " << sourcepath.c_str();
        return ret;
    }

    fd = ret;

    ret = fsptr_->Write(fd, page, 0, poolOpt_.metaPageSize);
    if (ret != static_cast<int>(poolOpt_.metaPageSize)) {
        fsptr_->Close(fd);
        LOG(ERROR) << "write metapage failed, " << sourcepath.c_str();
        return ret;
    }

    ret = fsptr_->Fsync(fd);
    if (ret != 0) {
        fsptr_->Close(fd);
        LOG(ERROR) << "fsync metapage failed, " << sourcepath.c_str();
        return ret;
    }

    ret = fsptr_->Close(fd);
    if (ret != 0) {
        LOG(ERROR) << "close failed, " << sourcepath.c_str();
        return ret;
    }
    return ret;
}

int FilePool::RecycleFile(const std::string &chunkpath) {
    if (!poolOpt_.getFileFromPool) {
        int ret = fsptr_->Delete(chunkpath.c_str());
        if (ret < 0) {
            LOG(ERROR) << "Recycle chunk failed!";
            return -1;
        }
    } else {
        // Check whether the size of the file to be recovered meets the
        // requirements, and delete it if it does not
        uint64_t chunklen = poolOpt_.fileSize + poolOpt_.metaPageSize;
        int fd = fsptr_->Open(chunkpath.c_str(), O_RDWR);
        if (fd < 0) {
            LOG(ERROR) << "file open failed! delete file dirctly"
                       << ", filename = " << chunkpath.c_str();
            return fsptr_->Delete(chunkpath.c_str());
        }

        struct stat info;
        int ret = fsptr_->Fstat(fd, &info);
        if (ret != 0) {
            LOG(ERROR) << "Fstat file " << chunkpath.c_str()
                       << "failed, ret = " << ret << ", delete file dirctly";
            fsptr_->Close(fd);
            return fsptr_->Delete(chunkpath.c_str());
        }

        if (info.st_size != static_cast<int64_t>(chunklen)) {
            LOG(ERROR) << "file size illegal, " << chunkpath.c_str()
                       << ", delete file dirctly"
                       << ", standard size = " << chunklen
                       << ", current file size = " << info.st_size;
            fsptr_->Close(fd);
            return fsptr_->Delete(chunkpath.c_str());
        }

        fsptr_->Close(fd);

        uint64_t newfilenum = 0;
        std::string newfilename;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            currentmaxfilenum_.fetch_add(1);
            newfilenum = currentmaxfilenum_.load();
            newfilename = std::to_string(newfilenum);
        }
        std::string targetpath = currentdir_ + "/" + newfilename;

        ret = fsptr_->Rename(chunkpath.c_str(), targetpath.c_str());
        if (ret < 0) {
            LOG(ERROR) << "file rename failed, " << chunkpath.c_str();
            return -1;
        } else {
            LOG(INFO) << "Recycle " << chunkpath.c_str() << ", success!"
                      << ", now chunkpool size = "
                      << currentState_.dirtyChunksLeft + 1;
        }
        std::unique_lock<std::mutex> lk(mtx_);
        dirtyChunks_.push_back(newfilenum);
        currentState_.dirtyChunksLeft++;
        currentState_.preallocatedChunksLeft++;
    }
    return 0;
}

void FilePool::UnInitialize() {
    currentdir_ = "";
    StopFormatting();
    std::unique_lock<std::mutex> lk(mtx_);
    dirtyChunks_.clear();
    cleanChunks_.clear();
}

bool FilePool::ScanInternal() {
    uint64_t maxnum = 0;
    std::vector<std::string> tmpvec;
    int ret = 0;
    LOG(INFO) << "scan dir" << currentdir_;
    if (!fsptr_->DirExists(currentdir_.c_str())) {
        ret = fsptr_->Mkdir(currentdir_.c_str());
        if (ret != 0) {
            LOG(ERROR) << "Mkdir [" << currentdir_ << "]"
                       << " failed!";
            return false;
        }
    }
    ret = fsptr_->List(currentdir_.c_str(), &tmpvec);
    if (ret < 0) {
        LOG(ERROR) << "list file pool dir failed!";
        return false;
    } else {
        LOG(INFO) << "list file pool dir done, size = " << tmpvec.size();
    }

    size_t suffixLen = kCleanChunkSuffix_.size();
    uint64_t chunklen = poolOpt_.fileSize + poolOpt_.metaPageSize;
    for (auto &iter : tmpvec) {
        bool isCleaned = false;
        std::string chunkNum = iter;
        if (::curve::common::StringEndsWith(iter, kCleanChunkSuffix_)) {
            isCleaned = true;
            chunkNum = iter.substr(0, iter.size() - suffixLen);
        }

        auto it =
            std::find_if(chunkNum.begin(), chunkNum.end(),
                         [](unsigned char c) { return !std::isdigit(c); });
        if (it != chunkNum.end()) {
            LOG(ERROR) << "file name illegal! [" << iter << "]";
            return false;
        }

        std::string filepath = currentdir_ + "/" + iter;
        if (!fsptr_->FileExists(filepath)) {
            LOG(ERROR) << "chunkfile pool dir has subdir! " << filepath.c_str();
            return false;
        }
        int fd = fsptr_->Open(filepath.c_str(), O_RDWR);
        if (fd < 0) {
            LOG(ERROR) << "file open failed!";
            return false;
        }
        struct stat info;
        int ret = fsptr_->Fstat(fd, &info);

        if (ret != 0 || info.st_size != static_cast<int64_t>(chunklen)) {
            LOG(ERROR) << "file size illegal, " << filepath.c_str()
                       << ", standard size = " << chunklen
                       << ", current size = " << info.st_size;
            fsptr_->Close(fd);
            return false;
        }

        fsptr_->Close(fd);
        uint64_t filenum = atoll(chunkNum.c_str());
        if (filenum != 0) {
            if (isCleaned) {
                cleanChunks_.push_back(filenum);
            } else {
                dirtyChunks_.push_back(filenum);
            }
            if (filenum > maxnum) {
                maxnum = filenum;
            }
        }
    }

    currentState_.chunkNum = tmpvec.size();
    currentState_.chunkNum += CountAllocatedNum(poolOpt_.copysetDir);
    currentState_.chunkNum += CountAllocatedNum(poolOpt_.recycleDir);

    std::unique_lock<std::mutex> lk(mtx_);
    currentmaxfilenum_.store(maxnum + 1);
    currentState_.dirtyChunksLeft = dirtyChunks_.size();
    currentState_.cleanChunksLeft = cleanChunks_.size();
    currentState_.preallocatedChunksLeft =
        currentState_.dirtyChunksLeft + currentState_.cleanChunksLeft;

    LOG(INFO) << "scan done, pool size = "
              << currentState_.preallocatedChunksLeft;
    return true;
}

uint64_t FilePool::CountAllocatedNum(const std::string& path) {
    std::vector<std::string> files;
    if ("" == path || 0 != fsptr_->List(path, &files)) {
        LOG(ERROR) << "FilePool failed to list files in " << path;
        return 0;
    }

    // Traverse subdirectories
    uint32_t chunkNum = 0;
    for (auto& file : files) {
        std::string filePath = path + "/" + file;
        bool isDir = fsptr_->DirExists(filePath);
        if (!isDir) {
            LOG(INFO) << "path = " << filePath;
            if (poolOpt_.isAllocated(file)) {
                ++chunkNum;
            }
        } else {
            chunkNum += CountAllocatedNum(filePath);
        }
    }
    return chunkNum;
}

size_t FilePool::Size() {
    std::unique_lock<std::mutex> lk(mtx_);
    return currentState_.preallocatedChunksLeft;
}

bool FilePool::EnoughChunk() {
    return Size() >= poolOpt_.chunkReserved;
}

FilePoolState FilePool::GetState() const {
    return currentState_;
}

const ChunkFormatStat& FilePool::GetChunkFormatStat() const {
    return formatStat_;
}

uint32_t FilePoolMeta::Crc32() const {
    const size_t size = sizeof(kFilePoolMagic) + sizeof(chunkSize) +
                        sizeof(metaPageSize) + filePoolPath.size() +
                        (hasBlockSize ? sizeof(blockSize) : 0);

    std::unique_ptr<char[]> crc(new char[size]);
    size_t off = 0;

    memcpy(crc.get(), kFilePoolMagic, sizeof(kFilePoolMagic));
    off += sizeof(kFilePoolMagic);

    memcpy(crc.get() + off, &chunkSize, sizeof(chunkSize));
    off += sizeof(chunkSize);

    memcpy(crc.get() + off, &metaPageSize, sizeof(metaPageSize));
    off += sizeof(metaPageSize);

    if (hasBlockSize) {
        memcpy(crc.get() + off, &blockSize, sizeof(blockSize));
        off += sizeof(blockSize);
    }

    memcpy(crc.get() + off, filePoolPath.c_str(), filePoolPath.size());
    off += filePoolPath.size();

    assert(off == size);
    return curve::common::CRC32(crc.get(), off);
}

}  // namespace chunkserver
}  // namespace curve
