#define LOG_NDEBUG 0
#define LOG_TAG "C2VdecBlockPoolUtil"

#include <C2VdecBlockPoolUtil.h>
#include <C2PlatformSupport.h>
#include <C2BlockInternal.h>
#include <C2BufferPriv.h>
#include <C2Buffer.h>
#include <utility>
#include <logdebug.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <iostream>
#include <algorithm>

#include <bufferpool/BufferPoolTypes.h>

using ::android::hardware::media::bufferpool::BufferPoolData;
namespace android {


const size_t kDefaultFetchGraphicBlockDelay = 10; // Default smoothing margin for dequeue block.
                                                  // kDefaultSmoothnessFactor + 2

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
        bool useSurface = C2PlatformAllocatorStore::BUFFERQUEUE == base->getAllocatorId();
        std::shared_ptr<C2AllocatorStore> allocatorStore = GetCodec2PlatformAllocatorStore();
        c2_status_t status = allocatorStore->fetchAllocator(base->getAllocatorId(), &mAllocatorBase);

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
        CODEC2_LOG(CODEC2_LOG_INFO, "Create block pool success, allocatorId:%d poolId:%llu use surface:%d",
            mBase->getAllocatorId(), mBase->getLocalId(), useSurface);
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

C2VdecBlockPoolUtil::C2VdecBlockPoolUtil(std::shared_ptr<C2BlockPool> blockPool)
    : mBlockingPool(std::make_shared<BlockingBlockPool>(blockPool)),
      mGraphicBufferId(0),
      mMaxDequeuedBufferNum(0) {
    mUseSurface = blockPool->getAllocatorId() == C2PlatformAllocatorStore::BUFFERQUEUE;
    CODEC2_LOG(CODEC2_LOG_INFO,"[%s] BlockPool id:%llu use surface:%d", __func__, blockPool->getLocalId(), mUseSurface);
}

C2VdecBlockPoolUtil::~C2VdecBlockPoolUtil() {
    mGraphicBufferId = 0;

    auto iter = mRawGraphicBlockInfo.begin();
    for (;iter != mRawGraphicBlockInfo.end(); iter++) {

        CODEC2_LOG(CODEC2_LOG_INFO, "[%s] block id:%d fd:%d dupFd:%d use count:%ld", __func__,
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
}

c2_status_t C2VdecBlockPoolUtil::fetchGraphicBlock(uint32_t width, uint32_t height, uint32_t format,
        C2MemoryUsage usage,
        std::shared_ptr<C2GraphicBlock> *block /* nonnull */) {
    ALOG_ASSERT(block != nullptr);
    ALOG_ASSERT(mBlockingPool != nullptr);

    CODEC2_LOG(CODEC2_LOG_INFO, "[%s] (%dx%d) block,current block size:%d max:%d ", __func__, width, height, mRawGraphicBlockInfo.size(), mMaxDequeuedBufferNum);

    std::lock_guard<std::mutex> lock(mMutex);
    std::shared_ptr<C2GraphicBlock> fetchBlock;
    c2_status_t err = C2_TIMED_OUT;
    if (mNextFetchTimeUs != 0) {
        int delayUs = GetNowUs() - mNextFetchTimeUs;
        if (delayUs > 0) {
            ::usleep(delayUs);
        }
        mNextFetchTimeUs = 0;
    }

    err = mBlockingPool->fetchGraphicBlock(width, height, format, usage, &fetchBlock);
    if (err == C2_OK) {
        ALOG_ASSERT(fetchBlock != nullptr);
        uint64_t inode;
        int fd = fetchBlock->handle()->data[0];
        getINodeFromFd(fd, &inode);
        auto iter = mRawGraphicBlockInfo.find(inode);
        if (iter != mRawGraphicBlockInfo.end()) {
            struct BlockBufferInfo info = mRawGraphicBlockInfo[inode];
            CODEC2_LOG(CODEC2_LOG_INFO, "Fetch block success, block inode: %llu fd:%d --> %d id:%d", inode, info.mFd, fd, info.mBlockId);
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
                    CODEC2_LOG(CODEC2_LOG_INFO, "Current block info size:%d", mRawGraphicBlockInfo.size());
                    fetchBlock.reset();
                    mNextFetchTimeUs = GetNowUs();
                    return C2_BLOCKING;
                }
            }
        }
    }
    else if (err == C2_TIMED_OUT) {
        CODEC2_LOG(CODEC2_LOG_INFO, "Fetch block time out and try again now.");
        mNextFetchTimeUs = GetNowUs();
        return err;
    }
    else {
        CODEC2_LOG(CODEC2_LOG_INFO, "No buffer could be recycled now, state:%d", err);
        mNextFetchTimeUs = GetNowUs();
        return err;
    }

    mNextFetchTimeUs = 0;
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

    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Block pool deque buffer number max:%d", mMaxDequeuedBufferNum);
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
    CODEC2_LOG(CODEC2_LOG_INFO,"[%s] Reset block block id:%d ret:%d", __func__, blockId,ret);
    return ret;
}

c2_status_t C2VdecBlockPoolUtil::resetGraphicBlock(std::shared_ptr<C2GraphicBlock> block) {
    std::lock_guard<std::mutex> lock(mBlockBufferMutex);
    int fd = block->handle()->data[0];

    auto blockInfo = std::find_if(mRawGraphicBlockInfo.begin(), mRawGraphicBlockInfo.end(),
        [fd, this](const std::pair<uint64_t, BlockBufferInfo> &gb) {
        if (mUseSurface)
            return gb.second.mDupFd == fd;
        else
            return gb.second.mFd == fd;
    });

    if (blockInfo != mRawGraphicBlockInfo.end()) {
        uint32_t blockId = blockInfo->second.mBlockId;
        return resetGraphicBlock(blockId);
    } else {
        CODEC2_LOG(CODEC2_LOG_INFO,"[%s] Reset but the block not found.", __func__);
        return C2_BAD_VALUE;
    }

    return C2_OK;
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
    ALOG_ASSERT(block != nullptr);

    int fd = block->handle()->data[0];
    uint64_t inode;
    getINodeFromFd(fd, &inode);
    auto info = mRawGraphicBlockInfo.find(inode);
    if (info == mRawGraphicBlockInfo.end()) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Get block id failed,this is unknow block");
        return C2_BAD_VALUE;
    }

    *blockId = info->second.mBlockId;
    CODEC2_LOG(CODEC2_LOG_INFO,"Get block id:%d", *blockId);
    return C2_OK;
}

c2_status_t C2VdecBlockPoolUtil::getPoolId(C2BlockPool::local_id_t *poolId) {
    ALOG_ASSERT(mBlockingPool != nullptr);

    if (!mBlockingPool) {
        CODEC2_LOG(CODEC2_LOG_ERR, "C2BufferQueueBlockPool Pool is Invalid, get pool id fail");
        return C2_BAD_VALUE;
    }

    *poolId = mBlockingPool->getLocalId();
    CODEC2_LOG(CODEC2_LOG_INFO, "[%s] pool id:%llu\n", __func__, *poolId);
    return C2_OK;
}

c2_status_t C2VdecBlockPoolUtil::getBlockFd(std::shared_ptr<C2GraphicBlock> block, int *fd) {
    ALOG_ASSERT(block != nullptr);

    uint64_t inode;
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
        CODEC2_LOG(CODEC2_LOG_ERR, "[%s] Block is null", __func__);
        return C2_BAD_VALUE;
    }
    struct BlockBufferInfo info;
    std::lock_guard<std::mutex> lock(mBlockBufferMutex);
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
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Fetch the new block(ino:%llu fd:%d dupfd:%d id:%d) and append.", inode, info.mFd, info.mDupFd, info.mBlockId);
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
    CODEC2_LOG(CODEC2_LOG_INFO,"[%s] get block id:%d", __func__, *blockId);
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

        CODEC2_LOG(CODEC2_LOG_INFO,"[%s:%d] ino(%llu) block(%d) fd(%d) dupfd(%d) use count(%ld)",__func__, __LINE__, info->first,
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
    auto blockIter = std::find_if(mRawGraphicBlockInfo.begin(), mRawGraphicBlockInfo.end(),
        [blockId](const std::pair<uint64_t, BlockBufferInfo> &gb) {
        return gb.second.mBlockId == blockId;
    });

    if (blockIter != mRawGraphicBlockInfo.end()) {
        CODEC2_LOG(CODEC2_LOG_INFO, "[%s] get block %d inode:%llu", __func__, blockId, blockIter->first);
        return blockIter->first;
    }

    return 0;
}

}
