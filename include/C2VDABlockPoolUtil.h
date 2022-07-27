#ifndef _C2_VDA_BLOCK_POOL_UTIL_H_
#define _C2_VDA_BLOCK_POOL_UTIL_H_

#include <errno.h>
#include <map>
#include <mutex>
#include <functional>

#include <android/hardware/graphics/bufferqueue/2.0/IGraphicBufferProducer.h>

#include <C2Buffer.h>
#include <C2BufferPriv.h>
#include <C2BqBufferPriv.h>
#include <ui/GraphicBuffer.h>
#include <C2PlatformSupport.h>
#include <ui/BufferQueueDefs.h>

namespace android {

// The wait time for another try to fetch a buffer from pool.
const int64_t kFetchRetryDelayUs = 10 * 1000;

class C2VDABlockPoolUtil {
public:
    explicit C2VDABlockPoolUtil(std::shared_ptr<C2BlockPool> blockpool);
    ~C2VDABlockPoolUtil();

    /**
     * Tries to dequeue a buffer from pool. If the new buffer is not in the mRawGraphicBlockInfo,
     * it is added to the mRawGraphicBlockInfo.
     * \param with the width of block buffer
     * \param height the height of block buffer
     * \param usage use usage
     * \param format the format of block buffer
    */
    c2_status_t fetchGraphicBlock(uint32_t width, uint32_t height, uint32_t format,
                                  C2MemoryUsage usage,
                                  std::shared_ptr<C2GraphicBlock>* block /* nonnull */);

    /**
     * Set maxDequeuedBufferCount as the requested buffer count to producer.
     *
     * \param bufferCount  the number of requested buffers
     */
    c2_status_t requestNewBufferSet(int32_t bufferCount);

    /**
     *  retset graphic block
     * \param blockId the id of block.
    */
    c2_status_t resetGraphicBlock(int32_t blockId);

    /**
     * Get the usage from block pool.
    */
    uint64_t getConsumerUsage();

    /**
     * Get the number of buffer on display.
     * \param minBuffersForDisplay the number of display.
    */
    c2_status_t getMinBuffersForDisplay(size_t* minBuffersForDisplay);

    /**
     * Get the block id from buffer pool.
     *
     * \param block graphic block.
     * \param blockId  block id.
     */
    c2_status_t getBlockIdByGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t* blockId);

    /**
     * Get the buffer pool id.
     *
     * \param poolId the id of pool
    */
    c2_status_t getPoolId(C2BlockPool::local_id_t *poolId);

    /**
     * Get the fd of buffer.
     *
     * \param block graphic block.
     * \param fd  the block fd.
    */
    c2_status_t getBlockFd(std::shared_ptr<C2GraphicBlock> block, int *fd);

    /**
     * Get the pool allocator id.
     */
    C2Allocator::id_t getAllocatorId();

    /**
    * is buffer queue pool.
    */
    bool isBufferQueue();

    /**
     * @brief clear all graphic block.
     */
    void  cancelAllGraphicBlock();

    /**
     * @brief  get block inode.
     * @param  blockId the block id.
     */
    uint64_t getBlockInodeByBlockId(uint32_t blockId);

private:

    /**
     * Add fetch new block to mRawGraphicBlockInfo.
     *
     * \param block graphic block.
     * \param inode  the node of block.
     * \param fd the fd of block.
    */
    c2_status_t appendOutputGraphicBlock(std::shared_ptr<C2GraphicBlock> block,uint64_t inode, int fd);

     /**
     * get block id from graphic block.
     *
     * \param block graphic block.
     * \param blockid the block id.
    */
    c2_status_t getBlockIdFromGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t* blockId);

    class BlockingBlockPool;
    std::shared_ptr<BlockingBlockPool> mBlockingPool;

    // The block buffer id of bufferqueue.
    int32_t mGraphicBufferId;
    std::mutex mMutex;

    // The Maximum number of buffers requested.
    size_t mMaxDequeuedBufferNum;

    // The indicator of whether buffer pool is used usrface.
    bool mUseSurface;

    // Internal struct to keep the information of a specific buffer.
    struct BlockBufferInfo {
        int mFd = -1;
        int mDupFd = -1;
        uint32_t mBlockId;
        std::shared_ptr<C2GraphicBlock> mGraphicBlock;
    };
    std::mutex mBlockBufferMutex;

    // The map of storing fetch output buffer information.
    std::map<uint64_t, BlockBufferInfo> mRawGraphicBlockInfo;

    // The timestamp for the next fetchGraphicBlock() call.
    // Set when the previous fetchGraphicBlock() call timed out.
    int64_t mNextFetchTimeUs GUARDED_BY(mMutex){0};
};

}

#endif //_C2_VDA_BLOCK_POOl_UTIL_H_
