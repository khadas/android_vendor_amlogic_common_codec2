/*
 * Copyright (C) 2023 Amlogic, Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_NDEBUG 0
#define LOG_TAG "C2VdecBlockPoolUtil"

#include <C2VdecBlockPoolUtil.h>
#include <C2PlatformSupport.h>
#include <C2BlockInternal.h>
#include <C2BufferPriv.h>
#include <C2Buffer.h>
#include <utility>
#include <C2VendorProperty.h>
#include <c2logdebug.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <iostream>
#include <algorithm>
#include <exception>

#include <bufferpool/BufferPoolTypes.h>

using ::android::hardware::media::bufferpool::BufferPoolData;
namespace android {


const size_t kDefaultFetchGraphicBlockDelay = 10; // Default smoothing margin for dequeue block.
                                                  // kDefaultSmoothnessFactor + 2
const size_t kDefaultDequeueBlockCountMax = 64;
int64_t GetNowUs() {
    struct timespec t;
    t.tv_sec = 0;
    t.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t nsecs = static_cast<int64_t>(t.tv_sec) * 1000000000LL + t.tv_nsec;
    return nsecs / 1000ll;
}

bool getINodeFromFd(int32_t fd, uint64_t *ino) {
    struct stat st;
    int ret = fstat(fd, &st);
    if (ret == -1) {
        ALOGE("Fstat error %d :%s", fd, strerror(errno));
        return false;
    } else {
        *ino = st.st_ino;
        //ALOGV("[%d]==fstat: st_ino:%llu st_uid:%u st_gid:%u", fd, st.st_ino, st.st_uid, st.st_gid);
    }
    return true;
}

class C2VdecBlockPoolUtil::BlockingBlockPool
{
public:
    BlockingBlockPool(const std::shared_ptr<C2BlockPool> &base) {
        C2Allocator::id_t id = base->getAllocatorId();
        if (id == C2Allocator::BAD_ID) {
            CODEC2_LOG(CODEC2_LOG_ERR, "[%s] got allocator id failed.", __func__);
            return;
        }

        bool useSurface = C2PlatformAllocatorStore::BUFFERQUEUE == id;
        std::shared_ptr<C2AllocatorStore> allocatorStore = GetCodec2PlatformAllocatorStore();
        c2_status_t status = allocatorStore->fetchAllocator(id, &mAllocatorBase);
        propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);

        if (status != C2_OK) {
            CODEC2_LOG(CODEC2_LOG_ERR, "Create block block pool fail.");
            return;
        }

        if (useSurface)
            mBase = base;
        else {
            mBase = std::make_shared<C2PooledBlockPool> (mAllocatorBase, base->getLocalId());
            C2String name = mAllocatorBase->getName();
            C2Allocator::id_t id = mAllocatorBase->getId();
            CODEC2_LOG(CODEC2_LOG_INFO, "Allocate name:%s id:%d", name.c_str(), id);
        }
        CODEC2_LOG(CODEC2_LOG_INFO, "Create block pool success, allocatorId:%d poolId:%d use surface:%d",
            mBase->getAllocatorId(), (int)mBase->getLocalId(), useSurface);
    }

    C2BlockPool::local_id_t getLocalId() {
        return mBase->getLocalId();
    }

    C2Allocator::id_t getAllocatorId() {
        return mBase->getAllocatorId();
    }

    c2_status_t fetchLinearBlock(
        uint32_t capacity,
        C2MemoryUsage usage,
        std::shared_ptr<C2LinearBlock> *block) {
        return mBase->fetchLinearBlock(capacity, usage, block);
    }

    c2_status_t fetchCircularBlock(
        uint32_t capacity,
        C2MemoryUsage usage,
        std::shared_ptr<C2CircularBlock> *block) {
        return mBase->fetchCircularBlock(capacity, usage, block);
    }

    c2_status_t fetchGraphicBlock(
        uint32_t width, uint32_t height, uint32_t format,
        C2MemoryUsage usage,
        std::shared_ptr<C2GraphicBlock> *block) {
        return mBase->fetchGraphicBlock(width, height, format, usage,
                                            block);
    }

    uint64_t getConsumerUsage() {
        uint64_t usage = 0;
        auto bq = std::static_pointer_cast<C2BufferQueueBlockPool>(mBase);
        bq->getConsumerUsage(&usage);
        return usage;
    }

    void resetPool(std::shared_ptr<C2BlockPool> blockPool) {
        mBase.reset();
        mBase = NULL;
        mBase = blockPool;
    }

private:
    std::shared_ptr<C2BlockPool> mBase;
    std::shared_ptr<C2Allocator> mAllocatorBase;
};

C2VdecBlockPoolUtil::C2VdecBlockPoolUtil(std::shared_ptr<C2BlockPool> blockPool) {
    mBlockingPool = std::make_shared<BlockingBlockPool> (blockPool);
    mGraphicBufferId = 0;
    mMaxDequeuedBufferNum = 0;
    mFetchBlockCount = 0;
    mFetchBlockSuccessCount = 0;
    C2Allocator::id_t id = blockPool->getAllocatorId();
    if (C2Allocator::BAD_ID == id) {
        CODEC2_LOG(CODEC2_LOG_ERR, "[%s] got allocator id failed.", __func__);
        return;
    }
    mUseSurface = (id == C2PlatformAllocatorStore::BUFFERQUEUE);
    CODEC2_LOG(CODEC2_LOG_INFO,"pool id:%" PRId64 " use surface:%d", blockPool->getLocalId(), mUseSurface);
}

C2VdecBlockPoolUtil::~C2VdecBlockPoolUtil() {
    mGraphicBufferId = 0;
    auto iter = mRawGraphicBlockInfo.begin();
    float fetchBlockLevel =  (float)mFetchBlockSuccessCount/(float)mFetchBlockCount;
    for (;iter != mRawGraphicBlockInfo.end(); iter++) {
        CODEC2_LOG(CODEC2_LOG_INFO, "~C2VdecBlockPoolUtil block id:%d fd:%d dupFd:%d use count:%ld",
            iter->second.mBlockId, iter->second.mFd, iter->second.mDupFd,
            iter->second.mGraphicBlock.use_count());

        if (iter->second.mFd >= 0) {
            close(iter->second.mFd);
            iter->second.mFd = -1;
        }

        if (iter->second.mDupFd >= 0) {
            close(iter->second.mDupFd);
            iter->second.mDupFd = -1;
        }
        iter->second.mGraphicBlock.reset();
    }
    mRawGraphicBlockInfo.clear();
    if (mBlockingPool != nullptr) {
        mBlockingPool.reset();
        mBlockingPool = nullptr;
    }

    CODEC2_LOG(CODEC2_LOG_INFO, "~C2VdecBlockPoolUtil success:%" PRId64 " count:%" PRId64 " fetch level:%f ", mFetchBlockSuccessCount, mFetchBlockCount, fetchBlockLevel);
}

c2_status_t C2VdecBlockPoolUtil::fetchGraphicBlock(uint32_t width, uint32_t height, uint32_t format,
        C2MemoryUsage usage,
        std::shared_ptr<C2GraphicBlock> *block /* nonnull */) {
    ALOG_ASSERT(block != nullptr);
    ALOG_ASSERT(mBlockingPool != nullptr);
    std::lock_guard<std::mutex> lock(mMutex);
    std::shared_ptr<C2GraphicBlock> fetchBlock;

    if (mUseSurface && (mRawGraphicBlockInfo.size() >= kDefaultDequeueBlockCountMax)) {
        CODEC2_LOG(CODEC2_LOG_ERR, "cancel fetch block. please check the number of block.");
        return C2_BLOCKING;
    }
    mFetchBlockCount ++;
    c2_status_t err = mBlockingPool->fetchGraphicBlock(width, height, format, usage, &fetchBlock);
    if (err == C2_OK) {
        ALOG_ASSERT(fetchBlock != nullptr);
        uint64_t inode = 0;
        int fd = fetchBlock->handle()->data[0];
        mFetchBlockSuccessCount++;
        getINodeFromFd(fd, &inode);
        //Scope of mBlockBufferMutex start
        {
            std::lock_guard<std::mutex> lock(mBlockBufferMutex);
            auto iter = mRawGraphicBlockInfo.find(inode);
            if (iter != mRawGraphicBlockInfo.end()) {
                struct BlockBufferInfo info = iter->second;
                int32_t blockInfoSize = mRawGraphicBlockInfo.size();
                CODEC2_LOG(CODEC2_LOG_TAG_BUFFER, "Fetch block success, current block inode:%" PRId64" fd:%d -> %d id:%d BlockInfoSize:%d Max:%d",
                    inode, info.mFd, fd, info.mBlockId, blockInfoSize, mMaxDequeuedBufferNum);
            } else {
                if (mUseSurface) {
                    c2_status_t ret = appendOutputGraphicBlock(fetchBlock, inode, fd);
                    if (ret != C2_OK) {
                        return ret;
                    }
                } else {
                    if (mRawGraphicBlockInfo.size() < mMaxDequeuedBufferNum) {
                        c2_status_t ret = appendOutputGraphicBlock(fetchBlock, inode, fd);
                        if (ret != C2_OK) {
                            return ret;
                        }
                    }
                    else if (mRawGraphicBlockInfo.size() >= mMaxDequeuedBufferNum) {
                        CODEC2_LOG(CODEC2_LOG_TAG_BUFFER, "Current block info size:%d",(int)mRawGraphicBlockInfo.size());
                        fetchBlock.reset();
                        return C2_BLOCKING;
                    }
                }
            }
        }//Scope of mBlockBufferMutex end
    }
    else if (err == C2_TIMED_OUT) {
        CODEC2_LOG(CODEC2_LOG_TAG_BUFFER, "Fetch block time out and try again now.");
        return err;
    }
    else {
        CODEC2_LOG(CODEC2_LOG_TAG_BUFFER, "No buffer could be recycled now, state:%d", err);
        return err;
    }

    ALOG_ASSERT(fetchBlock != nullptr);
    *block = std::move(fetchBlock);
    return C2_OK;
}

c2_status_t C2VdecBlockPoolUtil::requestNewBufferSet(int32_t bufferCount) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (bufferCount <= 0) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Invalid requested buffer count:%d", bufferCount);
        return C2_BAD_VALUE;
    }
    if (mUseSurface) {
        mMaxDequeuedBufferNum = static_cast<size_t>(bufferCount) + kDefaultFetchGraphicBlockDelay;
    } else {
        mMaxDequeuedBufferNum = static_cast<size_t>(bufferCount) + kDefaultFetchGraphicBlockDelay - 2;
    }

    CODEC2_LOG(CODEC2_LOG_TAG_BUFFER, "Block pool deque buffer number max:%d", mMaxDequeuedBufferNum);
    return C2_OK;
}

c2_status_t C2VdecBlockPoolUtil::resetGraphicBlock(int32_t blockId) {
    c2_status_t ret = C2_BAD_VALUE;
    std::lock_guard<std::mutex> lock(mBlockBufferMutex);
    auto info = mRawGraphicBlockInfo.begin();
    for (;info != mRawGraphicBlockInfo.end(); info++) {
        if (info->second.mBlockId == blockId) {
            CODEC2_LOG(CODEC2_LOG_INFO,"[%s] Reset block id:%d fd:%d DupFd:%d", __func__,
                info->second.mBlockId, info->second.mFd, info->second.mDupFd);
            if (info->second.mFd >= 0) {
                close(info->second.mFd);
                info->second.mFd = -1;
            }

            if (info->second.mDupFd >= 0) {
                close(info->second.mDupFd);
                info->second.mDupFd = -1;
            }
            info->second.mGraphicBlock.reset();
            mRawGraphicBlockInfo.erase(info);
            ret = C2_OK;
            break;
        }
    }

    if (mRawGraphicBlockInfo.size() == 0) {
        mGraphicBufferId = 0;
    }
    return ret;
}

c2_status_t C2VdecBlockPoolUtil::resetGraphicBlock(std::shared_ptr<C2GraphicBlock> block) {

    int fd = block->handle()->data[0];
    uint32_t blockId = 0;
    //Scope of mBlockBufferMutex start
    {
        std::lock_guard<std::mutex> lock(mBlockBufferMutex);
        auto blockInfo = std::find_if(mRawGraphicBlockInfo.begin(), mRawGraphicBlockInfo.end(),
            [fd, this](const std::pair<uint64_t, BlockBufferInfo> &gb) {
            if (mUseSurface)
                return gb.second.mDupFd == fd;
            else
                return gb.second.mFd == fd;
        });

        if (blockInfo != mRawGraphicBlockInfo.end()) {
            blockId = blockInfo->second.mBlockId;
        } else {
            CODEC2_LOG(CODEC2_LOG_INFO,"[%s] Reset but the block not found.", __func__);
            return C2_BAD_VALUE;
        }
    }
    //Scope of mBlockBufferMutex end
    return resetGraphicBlock(blockId);
}

uint64_t C2VdecBlockPoolUtil::getConsumerUsage() {
    ALOG_ASSERT(mBlockingPool != nullptr);

    if (mUseSurface) {
        return mBlockingPool->getConsumerUsage();
    }
    return 0;
}

c2_status_t C2VdecBlockPoolUtil::getMinBuffersForDisplay(size_t *minBuffersForDisplay) {
    *minBuffersForDisplay = 6;
    return C2_OK;
}

c2_status_t C2VdecBlockPoolUtil::getBlockIdByGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t *blockId) {
    if (block == nullptr || blockId == nullptr) {
        CODEC2_LOG(CODEC2_LOG_ERR, "[%s] The block is null", __func__);
        return C2_BAD_VALUE;
    }
    std::lock_guard<std::mutex> lock(mBlockBufferMutex);
    int fd = block->handle()->data[0];
    uint64_t inode = 0;
    getINodeFromFd(fd, &inode);
    auto info = mRawGraphicBlockInfo.find(inode);
    if (info == mRawGraphicBlockInfo.end()) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Get block id failed,this is unknown block");
        return C2_BAD_VALUE;
    }

    *blockId = info->second.mBlockId;
    return C2_OK;
}

c2_status_t C2VdecBlockPoolUtil::getPoolId(C2BlockPool::local_id_t *poolId) {
    ALOG_ASSERT(mBlockingPool != nullptr);

    if (!mBlockingPool) {
        CODEC2_LOG(CODEC2_LOG_ERR, "C2BufferQueueBlockPool Pool is Invalid, get pool id fail");
        return C2_BAD_VALUE;
    }

    *poolId = mBlockingPool->getLocalId();
    return C2_OK;
}

c2_status_t C2VdecBlockPoolUtil::getBlockFd(std::shared_ptr<C2GraphicBlock> block, int *fd) {
    if (block == nullptr || fd == nullptr) {
        CODEC2_LOG(CODEC2_LOG_ERR, "[%s] The block is null", __func__);
        return C2_BAD_VALUE;
    }
    std::lock_guard<std::mutex> lock(mBlockBufferMutex);
    uint64_t inode = 0;
    getINodeFromFd(block->handle()->data[0], &inode);
    auto info = mRawGraphicBlockInfo.find(inode);
    if (info == mRawGraphicBlockInfo.end()) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Get fd fail, unknown block");
        return C2_BAD_VALUE;
    }

    *fd = info->second.mDupFd;
    return C2_OK;
}

c2_status_t C2VdecBlockPoolUtil::appendOutputGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint64_t inode, int fd) {
    if (block == nullptr) {
        CODEC2_LOG(CODEC2_LOG_ERR, "[%s] The block is null", __func__);
        return C2_BAD_VALUE;
    }

    struct BlockBufferInfo info = {-1, -1, 0, NULL};
        //std::lock_guard<std::mutex> lock(mBlockBufferMutex);
    if (mUseSurface) {
        info.mFd = fd;
        info.mDupFd = dup(block->handle()->data[0]);
        info.mBlockId = mGraphicBufferId;
        info.mGraphicBlock = block;
        mRawGraphicBlockInfo.insert(std::pair<uint64_t, BlockBufferInfo>(inode, info));
        mGraphicBufferId++;
    }
    else {
        info.mFd = -1;
        info.mDupFd = dup(block->handle()->data[0]);
        info.mBlockId = mGraphicBufferId;
        //info.mGraphicBlock = block;
        mRawGraphicBlockInfo.insert(std::pair<uint64_t, BlockBufferInfo>(inode, info));
        mGraphicBufferId++;
    }

    CODEC2_LOG(CODEC2_LOG_INFO, "Fetch the new block(ino:%" PRId64 " fd:%d dup fd:%d id:%d) and append.", inode, info.mFd, info.mDupFd, info.mBlockId);
    return C2_OK;
}

c2_status_t C2VdecBlockPoolUtil::getBlockIdFromGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t* blockId) {
    if (block == nullptr) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Block is null, get block id failed.");
        return C2_BAD_VALUE;
    }

    std::shared_ptr<_C2BlockPoolData> blockPoolData =
            _C2BlockFactory::GetGraphicBlockPoolData(*block);
    if (blockPoolData->getType() != _C2BlockPoolData::TYPE_BUFFERPOOL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Obtained C2GraphicBlock is not bufferpool-backed.");
        return C2_BAD_VALUE;
    }
    std::shared_ptr<BufferPoolData> bpData;
    if (!_C2BlockFactory::GetBufferPoolData(blockPoolData, &bpData) || !bpData) {
        CODEC2_LOG(CODEC2_LOG_ERR, "BufferPoolData unavailable in block.");
        return C2_BAD_VALUE;
    }
    *blockId = bpData->mId;
    CODEC2_LOG(CODEC2_LOG_TAG_BUFFER,"[%s] get block id:%d", __func__, *blockId);
    return C2_OK;
}

C2Allocator::id_t C2VdecBlockPoolUtil::getAllocatorId() {
    ALOG_ASSERT(mBlockingPool != nullptr);
    return mBlockingPool->getAllocatorId();
}

bool C2VdecBlockPoolUtil::isBufferQueue() {
    bool ret = C2PlatformAllocatorStore::BUFFERQUEUE == getAllocatorId();
    return ret;
}

void C2VdecBlockPoolUtil::cancelAllGraphicBlock() {
    std::lock_guard<std::mutex> lock(mBlockBufferMutex);
    auto info = mRawGraphicBlockInfo.begin();
    for (;info != mRawGraphicBlockInfo.end(); info++) {

        CODEC2_LOG(CODEC2_LOG_TAG_BUFFER,"[%s:%d] ino(%" PRId64")block(%d) fd(%d) dup fd(%d) use count(%ld)",__func__, __LINE__, info->first,
            info->second.mBlockId, info->second.mFd, info->second.mDupFd, info->second.mGraphicBlock.use_count());

        if (info->second.mFd >= 0) {
            close(info->second.mFd);
            info->second.mFd = -1;
        }

        if (info->second.mDupFd >= 0) {
            close(info->second.mDupFd);
            info->second.mDupFd = -1;
        }

        info->second.mBlockId = -1;
        info->second.mGraphicBlock.reset();
    }
    mGraphicBufferId = 0;
    mMaxDequeuedBufferNum = 0;
    mRawGraphicBlockInfo.clear();
}

uint64_t C2VdecBlockPoolUtil::getBlockInodeByBlockId(uint32_t blockId) {
    std::lock_guard<std::mutex> lock(mBlockBufferMutex);
    auto blockIter = std::find_if(mRawGraphicBlockInfo.begin(), mRawGraphicBlockInfo.end(),
        [blockId](const std::pair<uint64_t, BlockBufferInfo> &gb) {
        return gb.second.mBlockId == blockId;
    });

    if (blockIter != mRawGraphicBlockInfo.end()) {
        //CODEC2_LOG(CODEC2_LOG_TAG_BUFFER, "[%s] get block %d inode:%llu", __func__, blockId, blockIter->first);
        return blockIter->first;
    }

    return 0;
}

void C2VdecBlockPoolUtil::resetBlockPool(std::shared_ptr<C2BlockPool> blockPool) {
    if (blockPool == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "reset block pool failed");
    }
    mBlockingPool->resetPool(blockPool);
}

}
