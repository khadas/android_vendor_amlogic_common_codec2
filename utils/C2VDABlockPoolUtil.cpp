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

namespace android {

const size_t kDefaultFetchGraphicBlockDelay = 9; // kDefaultSmoothnessFactor + 2

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
    BlockingBlockPool(const std::shared_ptr<C2BlockPool> &base) : mBase{base} {}

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
        c2_status_t status;
        do {
            status = mBase->fetchLinearBlock(capacity, usage, block);
        } while (status == C2_BLOCKING);
        return status;
    }

    c2_status_t fetchCircularBlock(
        uint32_t capacity,
        C2MemoryUsage usage,
        std::shared_ptr<C2CircularBlock> *block) {
        c2_status_t status;
        do {
            status = mBase->fetchCircularBlock(capacity, usage, block);
        } while (status == C2_BLOCKING);
        return status;
    }

    c2_status_t fetchGraphicBlock(
        uint32_t width, uint32_t height, uint32_t format,
        C2MemoryUsage usage,
        std::shared_ptr<C2GraphicBlock> *block) {
        c2_status_t status;
        do {
            status = mBase->fetchGraphicBlock(width, height, format, usage,
                                              block);
        } while (status == C2_BLOCKING);
        return status;
    }

    uint64_t getConsumerUsage() {
        uint64_t usage = 0;
        auto bq = std::static_pointer_cast<C2BufferQueueBlockPool>(mBase);
        bq->getConsumerUsage(&usage);
        return usage;
    }

private:
    std::shared_ptr<C2BlockPool> mBase;
};

C2VDABlockPoolUtil::C2VDABlockPoolUtil(bool useSurface, std::shared_ptr<C2BlockPool> blockpool)
    : mBlockingPool(std::make_shared<BlockingBlockPool>(blockpool)),
      mGraphicBufferId(0),
      mMaxDequeuedBufferNum(0),
      mUseSurface(useSurface) {

    ALOGI("%s blockPool id:%llu", __func__, mBlockingPool->getLocalId());
}

C2VDABlockPoolUtil::~C2VDABlockPoolUtil() {
    ALOGV("%s", __func__);
    mGraphicBufferId = 0;
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
    while (1) {
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
                break;
            }
            else if (mRawGraphicBlockInfo.size() < mMaxDequeuedBufferNum) {
                appendOutputGraphicBlock(fetchBlock, inode, fd);
                break;
            }
        }
        else if (err == C2_TIMED_OUT) {
            ALOGE("Fetch block time out and try again...");
        }
        else {
            ALOGE("No buffer could be recycled now, wait for retry err = %d", err);
        }

        fetchBlock.reset();
        mNextFetchTimeUs = GetNowUs() + kFetchRetryDelayUs;
    }

    *block = std::move(fetchBlock);
    return C2_OK;
}

c2_status_t C2VDABlockPoolUtil::requestNewBufferSet(int32_t bufferCount) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (bufferCount <= 0) {
        ALOGE("Invalid requested buffer count:%d", bufferCount);
        return C2_BAD_VALUE;
    }
    mMaxDequeuedBufferNum = static_cast<size_t>(bufferCount) + kDefaultFetchGraphicBlockDelay;
    ALOGV("block pool deque buffer number max:%d", mMaxDequeuedBufferNum);
    return C2_OK;
}

c2_status_t C2VDABlockPoolUtil::resetGraphicBlock(int32_t blockId) {
    // TODO
    ALOGI("%s reset block success", __func__);
    return C2_OK;
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

    *fd = info->second.mDupFd;
    ALOGI("%s get fd success %d", __func__, *fd);
    return C2_OK;
}

void C2VDABlockPoolUtil::appendOutputGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint64_t inode, int fd) {
    ALOG_ASSERT(block != nullptr);

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
        // TODO
        // pool buffer
    }
    ALOGI("Fetch new block and append, block info: %llu fd:%d id:%d", inode, info.mFd, info.mBlockId);
}

}