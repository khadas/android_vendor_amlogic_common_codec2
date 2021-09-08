/*
 * Copyright 2018, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_C2_VDA_BQ_BLOCK_POOL_H_
#define ANDROID_C2_VDA_BQ_BLOCK_POOL_H_

#include <functional>
#include <map>

#include <android/hardware/graphics/bufferqueue/2.0/IGraphicBufferProducer.h>

#include <C2BqBufferPriv.h>
#include <C2Buffer.h>
#include <ui/GraphicBuffer.h>

#include <errno.h>

#include <mutex>

#include <ui/BufferQueueDefs.h>
#include <utils/Log.h>

#include <C2AllocatorGralloc.h>
#include <C2PlatformSupport.h>

using ::android::GraphicBuffer;
using ::android::BufferQueueDefs::NUM_BUFFER_SLOTS;


/**
 * The BufferQueue-backed block pool design which supports to request arbitrary count of graphic
 * buffers from IGBP, and use this buffer set among codec component and client.
 *
 * The block pool should restore the mapping table between slot indices and GraphicBuffer (or
 * C2GraphicAllocation). When component requests a new buffer, the block pool calls dequeueBuffer
 * to IGBP to obtain a valid slot index, and returns the corresponding buffer from map.
 *
 * Buffers in the map should be canceled to IGBP on block pool destruction, or on resolution change
 * request.
 */
class C2VdaBqBlockPool : public C2BufferQueueBlockPool {
public:
    //static std::map<C2Handle*, int32_t> mBlockAllocations;

    C2VdaBqBlockPool(std::shared_ptr<android::C2AllocatorGralloc> allocator, const local_id_t localId);

    ~C2VdaBqBlockPool() override;

    C2Allocator::id_t getAllocatorId() const override {/*ALOGI("get allocatorId %d", mAllocator->getId());*/ return mAllocator->getId(); };

    local_id_t getLocalId() const override { return mLocalId; };

    /**
     * Tries to dequeue a buffer from producer. If the dequeued slot is not in |mSlotBuffers| and
     * BUFFER_NEEDS_REALLOCATION is returned, allocates new buffer from producer by requestBuffer
     * and records the buffer and its slot index into |mSlotBuffers|.
     *
     * When the size of |mSlotBuffers| reaches the requested buffer count, set disallow allocation
     * to producer. After that only slots with allocated buffer could be dequeued.
     */
    virtual c2_status_t fetchGraphicBlock(uint32_t width, uint32_t height, uint32_t format,
                                  C2MemoryUsage usage,
                                  std::shared_ptr<C2GraphicBlock>* block /* nonnull */) override;

    typedef ::android::hardware::graphics::bufferqueue::V2_0::
            IGraphicBufferProducer HGraphicBufferProducer;

    void configureProducer(const android::sp<HGraphicBufferProducer>& producer) override;

     /**
     * Configures an IGBP in order to create blocks. A newly created block is
     * dequeued from the configured IGBP. Unique Id of IGBP and the slot number of
     * blocks are passed via native_handle. Managing IGBP is responsibility of caller.
     * When IGBP is not configured, block will be created via allocator.
     * Since zero is not used for Unique Id of IGBP, if IGBP is not configured or producer
     * is configured as nullptr, unique id which is bundled in native_handle is zero.
     *
     * \param producer      the IGBP, which will be used to fetch blocks
     * \param syncMemory    Shared memory for synchronization of allocation & deallocation.
     * \param bqId          Id of IGBP
     * \param generationId  Generation Id for rendering output
     * \param consumerUsage consumerUsage flagof the IGBP
     */
    virtual void configureProducer(
            const android::sp<HGraphicBufferProducer> &producer,
            native_handle_t *syncMemory,
            uint64_t bqId,
            uint32_t generationId,
            uint64_t consumerUsage);

    /**
     * Sends the request of arbitrary number of graphic buffers allocation. If producer is given,
     * it will set maxDequeuedBufferCount as the requested buffer count to producer.
     *
     * \note C2VdaBqBlockPool-specific function
     *
     * \param bufferCount  the number of requested buffers
     */
    c2_status_t requestNewBufferSet(int32_t bufferCount);

    c2_status_t getPoolIdFromGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t* poolId);

    c2_status_t resetGraphicBlock(int32_t slot);

    int64_t getSurfaceUsage();

    static c2_status_t getMinBuffersForDisplay(size_t* minBuffersForDisplay);
private:
    c2_status_t cancelAllBuffers();

    std::shared_ptr<android::C2AllocatorGralloc> mAllocator;
    const local_id_t mLocalId;

    android::sp<HGraphicBufferProducer> mProducer;
    uint64_t mProducerId;
    uint32_t mGeneration;

    // Function mutex to lock at the start of each API function call for protecting the
    // synchronization of all member variables.
    std::mutex mMutex;

    std::map<int32_t, std::shared_ptr<C2GraphicAllocation>> mSlotAllocations;
    //std::map<int32_t, android::sp<GraphicBuffer>> mSlotGraphicBuffers;
    std::map<C2GraphicBlock*, int32_t> mBlockAllocations;
    size_t mMaxDequeuedBuffers;

    uint64_t mConsumerUsage;

    android::sp<GraphicBuffer> mBuffers[NUM_BUFFER_SLOTS];

    std::weak_ptr<C2BufferQueueBlockPoolData> mPoolDatas[NUM_BUFFER_SLOTS];

    std::shared_ptr<C2SurfaceSyncMemory> mSyncMem;
};

#endif  // ANDROID_C2_VDA_BQ_BLOCK_POOL_H_
