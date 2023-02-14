/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */

#ifndef _C2_Vdec_BLOCK_POOL_UTIL_H_
#define _C2_Vdec_BLOCK_POOL_UTIL_H_

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

namespace android
{

// The wait time for another try to fetch a buffer from pool.
const int64_t kFetchRetryDelayUs = 10 * 1000;

class C2VdecBlockPoolUtil
{
public:
    explicit C2VdecBlockPoolUtil(std::shared_ptr<C2BlockPool> blockPool);
    ~C2VdecBlockPoolUtil();

    /**
     * @brief Tries to dequeue a buffer from pool. If the new buffer is not in the mRawGraphicBlockInfo,
     * it is added to the mRawGraphicBlockInfo.
     *
     * \param with   the width of block buffer
     * \param height the height of block buffer
     * \param usage  use usage
     * \param format the format of block buffer
     */
    c2_status_t fetchGraphicBlock(uint32_t width, uint32_t height, uint32_t format,
                                    C2MemoryUsage usage,
                                    std::shared_ptr<C2GraphicBlock> *block /* nonnull */);

    /**
     * @brief Set maxDequeuedBufferCount as the requested buffer count to producer.
     *
     * \param bufferCount  the number of requested buffers
     */
    c2_status_t requestNewBufferSet(int32_t bufferCount);

    /**
     * @brief Reset graphic block
     *
     * \param blockId the id of block.
     */
    c2_status_t resetGraphicBlock(int32_t blockId);

        /**
     * @brief Reset graphic block
     *
     * \param blockId the id of block.
     */
    c2_status_t resetGraphicBlock(std::shared_ptr<C2GraphicBlock> block);

    /**
     * @brief Get the usage from block pool.
     */
    uint64_t getConsumerUsage();

    /**
     * @brief Get the number of buffer on display.
     * \param minBuffersForDisplay the number of display.
     */
    c2_status_t getMinBuffersForDisplay(size_t *minBuffersForDisplay);

    /**
     * @brief Get the block id from buffer pool.
     *
     * \param block graphic block.
     * \param blockId  block id.
     */
    c2_status_t getBlockIdByGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t *blockId);

    /**
     * @brief Get the buffer pool id.
     *
     * \param poolId the id of pool
     */
    c2_status_t getPoolId(C2BlockPool::local_id_t *poolId);

    /**
     * @brief Get the fd of buffer.
     *
     * \param block graphic block.
     * \param fd    the block fd.
     */
    c2_status_t getBlockFd(std::shared_ptr<C2GraphicBlock> block, int *fd);

    /**
     * @brief Get the pool allocator id.
     */
    C2Allocator::id_t getAllocatorId();

    /**
     * @brief Is buffer queue pool.
     */
    bool isBufferQueue();

    /**
     * @brief clear all graphic block.
     */
    void cancelAllGraphicBlock();

    /**
     * @brief  get block inode.
     * @param  blockId the block id.
     */
    uint64_t getBlockInodeByBlockId(uint32_t blockId);

    void resetBlockPool(std::shared_ptr<C2BlockPool> blockPool);
private:
    /**
     * @brief Add fetch new block to mRawGraphicBlockInfo.
     *
     * \param block  graphic block.
     * \param inode  the node of block.
     * \param fd     the fd of block.
     */
    c2_status_t appendOutputGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint64_t inode, int fd);

    /**
     * @brief get block id from graphic block.
     *
     * \param block  graphic block.
     * \param blockId the block id.
     */
    c2_status_t getBlockIdFromGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t *blockId);

    class BlockingBlockPool;
    std::shared_ptr<BlockingBlockPool> mBlockingPool;

    // The block buffer id of bufferqueue.
    int32_t mGraphicBufferId;
    std::mutex mMutex;

    int32_t mMaxDequeuedBufferNum;

    // The indicator of whether buffer pool is used surface.
    bool mUseSurface = false;

    // Internal struct to keep the information of a specific buffer.
    struct BlockBufferInfo
    {
        int mFd;
        int mDupFd;
        uint32_t mBlockId;
        std::shared_ptr<C2GraphicBlock> mGraphicBlock;
    };
    std::mutex mBlockBufferMutex;
    // The map of storing fetch output buffer information.
    std::map<uint64_t, BlockBufferInfo> mRawGraphicBlockInfo;
    // This count is used to count the number of fetchblock.
    int64_t mFetchBlockCount;
    // This count is used to count the number of successful fetchblock.
    int64_t mFetchBlockSuccessCount;
};

}

#endif //_C2_Vdec_BLOCK_POOl_UTIL_H_
