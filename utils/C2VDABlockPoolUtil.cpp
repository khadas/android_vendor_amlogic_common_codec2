#define LOG_NDEBUG 0
#define LOG_TAG "C2VDABlockPoolUtil"

#include <C2VDABlockPoolUtil.h>
#include <C2PlatformSupport.h>
#include <C2BlockInternal.h>
#include <C2BufferPriv.h>
#include <C2Buffer.h>
#include <utility>
#include <logdebug.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include <bufferpool/BufferPoolTypes.h>

using ::android::hardware::media::bufferpool::BufferPoolData;
namespace android {


const size_t kDefaultFetchGraphicBlockDelay = 9; // Default smoothing margin for dequeue block.
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
    //ALOGV("getInode");
    struct stat st;
    int ret = fstat(fd, &st);
    if (ret == -1) {
        ALOGE("fstat error %d :%s", fd, strerror(errno));
        return false;
    }
    else {
        *ino = st.st_ino;
        ALOGV("[%d]==fstat: st_ino:%llu st_uid:%u st_gid:%u", fd, st.st_ino, st.st_uid, st.st_gid);
    }
    return true;
}

class C2VDABlockPoolUtil::BlockingBlockPool
{
public:
    BlockingBlockPool(const std::shared_ptr<C2BlockPool> &base) {
        bool useSurface = C2PlatformAllocatorStore::BUFFERQUEUE == base->getAllocatorId();
        std::shared_ptr<C2AllocatorStore> allocatorStore = GetCodec2PlatformAllocatorStore();
        c2_status_t status = allocatorStore->fetchAllocator(base->getAllocatorId(), &mAllocatorBase);

        if (status != C2_OK) {
            ALOGV("create block block pool fail.");
            return;
        }

        if (useSurface)
            mBase = base;
        else
            mBase = std::make_shared<C2PooledBlockPool> (mAllocatorBase, base->getLocalId());

        ALOGV("create block pool success, allocatorId:%d poolId:%llu use surface:%d",
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

    void resetPool(std::shared_ptr<C2BlockPool> blockpool) {
        mBase.reset();
        mBase = blockpool;
    }

private:
    std::shared_ptr<C2BlockPool> mBase;
    std::shared_ptr<C2Allocator> mAllocatorBase;
};

C2VDABlockPoolUtil::C2VDABlockPoolUtil(std::shared_ptr<C2BlockPool> blockpool)
    : mBlockingPool(std::make_shared<BlockingBlockPool>(blockpool)),
      mGraphicBufferId(0),
      mMaxDequeuedBufferNum(0) {
    mUseSurface = blockpool->getAllocatorId() == C2PlatformAllocatorStore::BUFFERQUEUE;
    ALOGI("%s blockPool id:%llu use surface:%d", __func__, blockpool->getLocalId(), mUseSurface);
}

C2VDABlockPoolUtil::~C2VDABlockPoolUtil() {
    ALOGV("%s", __func__);
    mGraphicBufferId = 0;

    auto iter = mRawGraphicBlockInfo.begin();
    for (;iter != mRawGraphicBlockInfo.end(); iter++) {
        ALOGE("%s graphicblock use count:%ld", __func__, iter->second.mGraphicBlock.use_count());
        if (iter->second.mFd >= 0) {
            close(iter->second.mFd);
        }

        if (iter->second.mDupFd >= 0) {
            close(iter->second.mDupFd);
        }
        iter->second.mGraphicBlock.reset();
    }

    mRawGraphicBlockInfo.clear();
    mBlockingPool.reset();
}

c2_status_t C2VDABlockPoolUtil::fetchGraphicBlock(uint32_t width, uint32_t height, uint32_t format,
        C2MemoryUsage usage,
        std::shared_ptr<C2GraphicBlock> *block /* nonnull */) {
    ALOG_ASSERT(block != nullptr);
    ALOG_ASSERT(mBlockingPool != nullptr);

    ALOGV("%s (%dx%d) block", __func__, width, height);

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
            ALOGV("Fetch used block success, block inode: %llu fd:%d --> %d id:%d", inode, info.mFd, fd, info.mBlockId);
        }
        else if (mRawGraphicBlockInfo.size() < mMaxDequeuedBufferNum) {
            c2_status_t ret = appendOutputGraphicBlock(fetchBlock, inode, fd);
            if (ret != C2_OK) {
                return ret;
            }
        }
        else if (mRawGraphicBlockInfo.size() >= mMaxDequeuedBufferNum) {
            fetchBlock.reset();
            mNextFetchTimeUs = GetNowUs();
            return C2_BLOCKING;
        }
    }
    else if (err == C2_TIMED_OUT) {
        ALOGE("Fetch block time out and try again...");
        mNextFetchTimeUs = GetNowUs();
        return err;
    }
    else {
        ALOGE("No buffer could be recycled now, err = %d", err);
        mNextFetchTimeUs = GetNowUs();
        return err;
    }

    mNextFetchTimeUs = 0;
    ALOG_ASSERT(fetchBlock != nullptr);
    *block = std::move(fetchBlock);
    return C2_OK;
}

c2_status_t C2VDABlockPoolUtil::requestNewBufferSet(int32_t bufferCount) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (bufferCount <= 0) {
        ALOGE("Invalid requested buffer count:%d", bufferCount);
        return C2_BAD_VALUE;
    }
    if (mUseSurface) {
        mMaxDequeuedBufferNum = static_cast<size_t>(bufferCount) + kDefaultFetchGraphicBlockDelay;
    } else {
        mMaxDequeuedBufferNum = static_cast<size_t>(bufferCount) + kDefaultFetchGraphicBlockDelay - 2;
    }

    ALOGV("block pool deque buffer number max:%d", mMaxDequeuedBufferNum);
    return C2_OK;
}

c2_status_t C2VDABlockPoolUtil::resetGraphicBlock(int32_t blockId) {
    c2_status_t ret = C2_BAD_VALUE;
    auto info = mRawGraphicBlockInfo.begin();
    for (;info != mRawGraphicBlockInfo.end(); info++) {
        if (info->second.mBlockId == blockId) {
            if (info->second.mFd >= 0) {
                close(info->second.mFd);
            }

            if (info->second.mDupFd >= 0) {
                close(info->second.mDupFd);
            }
            info->second.mGraphicBlock.reset();
            mRawGraphicBlockInfo.erase(info);
            ret = C2_OK;
            break;
        }
    }
    ALOGV("%s reset block blockId:%d ret:%d", __func__, blockId,ret);
    return ret;
}

uint64_t C2VDABlockPoolUtil::getConsumerUsage() {
    ALOG_ASSERT(mBlockingPool != nullptr);

    if (mUseSurface) {
        return mBlockingPool->getConsumerUsage();
    }
    return 0;
}

c2_status_t C2VDABlockPoolUtil::getMinBuffersForDisplay(size_t *minBuffersForDisplay) {
    ALOGV("getMinBuffersForDisplay\n");
    *minBuffersForDisplay = 6;
    return C2_OK;
}

c2_status_t C2VDABlockPoolUtil::getBlockIdByGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t *blockId) {
    ALOG_ASSERT(block != nullptr);

    int fd = block->handle()->data[0];
    uint64_t inode;
    getINodeFromFd(fd, &inode);
    auto info = mRawGraphicBlockInfo.find(inode);
    if (info == mRawGraphicBlockInfo.end()) {
        ALOGI("get block id failed,this is unknow block");
        return C2_BAD_VALUE;
    }

    *blockId = info->second.mBlockId;
    ALOGI("get block id success, id:%d", *blockId);
    return C2_OK;
}

c2_status_t C2VDABlockPoolUtil::getPoolId(C2BlockPool::local_id_t *poolId) {
    ALOG_ASSERT(mBlockingPool != nullptr);

    if (!mBlockingPool) {
        ALOGE("C2BufferQueueBlockPool Pool is Invalid, get pool id fail");
        return C2_BAD_VALUE;
    }

    *poolId = mBlockingPool->getLocalId();
    ALOGI("%s pool id:%llu\n", __func__, *poolId);
    return C2_OK;
}

c2_status_t C2VDABlockPoolUtil::getBlockFd(std::shared_ptr<C2GraphicBlock> block, int *fd) {
    ALOG_ASSERT(block != nullptr);

    uint64_t inode;
    getINodeFromFd(block->handle()->data[0], &inode);
    auto info = mRawGraphicBlockInfo.find(inode);
    if (info == mRawGraphicBlockInfo.end()) {
        ALOGI("get fd fail, unknown block");
        return C2_BAD_VALUE;
    }

    if (mUseSurface) {
        *fd = info->second.mDupFd;
    } else {
        *fd = info->second.mFd;
    }
    ALOGI("%s get fd success %d", __func__, *fd);
    return C2_OK;
}

c2_status_t C2VDABlockPoolUtil::appendOutputGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint64_t inode, int fd) {
    if (block == nullptr) {
        ALOGV("%s block is null", __func__);
        return C2_BAD_VALUE;
    }
    struct BlockBufferInfo info;
    if (mUseSurface) {
        info.mFd = fd;
        info.mDupFd = dup(block->handle()->data[0]);
        info.mBlockId = mGraphicBufferId;
        info.mGraphicBlock = block;
        mRawGraphicBlockInfo.insert(std::pair<uint64_t, BlockBufferInfo>(inode, info));
        mGraphicBufferId++;
    }
    else {
        info.mFd = fd;
        info.mDupFd = fd;//dup(block->handle()->data[0]);
        info.mBlockId = mGraphicBufferId;
        //info.mGraphicBlock = block;
        mRawGraphicBlockInfo.insert(std::pair<uint64_t, BlockBufferInfo>(inode, info));
        mGraphicBufferId++;
    }
    ALOGI("Fetch new block and append, block info: %llu fd:%d id:%d", inode, info.mFd, info.mBlockId);
    return C2_OK;
}

c2_status_t C2VDABlockPoolUtil::getBlockIdFromGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t* blockId) {
    if (block == nullptr) {
        ALOGE("block is null, get block id failed.");
        return C2_BAD_VALUE;
    }

    std::shared_ptr<_C2BlockPoolData> blockPoolData =
            _C2BlockFactory::GetGraphicBlockPoolData(*block);
    if (blockPoolData->getType() != _C2BlockPoolData::TYPE_BUFFERPOOL) {
        ALOGE("Obtained C2GraphicBlock is not bufferpool-backed.");
        return C2_BAD_VALUE;
    }
    std::shared_ptr<BufferPoolData> bpData;
    if (!_C2BlockFactory::GetBufferPoolData(blockPoolData, &bpData) || !bpData) {
        ALOGE("BufferPoolData unavailable in block.");
        return C2_BAD_VALUE;
    }
    *blockId = bpData->mId;
    ALOGV("%s get block id:%d", __func__, *blockId);
    return C2_OK;
}

C2Allocator::id_t C2VDABlockPoolUtil::getAllocatorId() {
    ALOG_ASSERT(mBlockingPool != nullptr);
    return mBlockingPool->getAllocatorId();
}

bool C2VDABlockPoolUtil::isBufferQueue() {
    bool ret = C2PlatformAllocatorStore::BUFFERQUEUE == getAllocatorId();
    ALOGV("%s ret:%d", __func__, ret);
    return ret;
}

void C2VDABlockPoolUtil::cancelAllGraphicBlock() {
    auto info = mRawGraphicBlockInfo.begin();
    for (;info != mRawGraphicBlockInfo.end(); info++) {
        if (info->second.mFd >= 0) {
            close(info->second.mFd);
        }

        if (info->second.mDupFd >= 0) {
            close(info->second.mDupFd);
        }

        info->second.mBlockId = -1;
        info->second.mGraphicBlock.reset();
    }
    mGraphicBufferId = 0;
    mMaxDequeuedBufferNum = 0;
    mRawGraphicBlockInfo.clear();
}

}
