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

#ifndef ANDROID_C2_VDA_POOLED_BLOCK_POOL_H_
#define ANDROID_C2_VDA_POOLED_BLOCK_POOL_H_

#include <functional>
#include <map>
#include <set>

#include <C2BufferPriv.h>
#include <C2Buffer.h>
#include <ui/GraphicBuffer.h>

#include <errno.h>

#include <mutex>

#include <utils/Log.h>

#include <C2AllocatorGralloc.h>
#include <C2PlatformSupport.h>

using ::android::GraphicBuffer;

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
class C2VdaPooledBlockPool : public C2PooledBlockPool {
public:
    //static std::map<C2Handle*, int32_t> mBlockAllocations;

    C2VdaPooledBlockPool(std::shared_ptr<android::C2AllocatorGralloc> allocator, const local_id_t localId);

    ~C2VdaPooledBlockPool() override;

    C2Allocator::id_t getAllocatorId() const override {/*ALOGI("get allocatorId %d", mAllocator->getId());*/ return mAllocator->getId();};

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

    c2_status_t requestNewBufferSet(int32_t bufferCount);

    c2_status_t getPoolIdFromGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t* poolId);

    c2_status_t resetGraphicBlock(int32_t slot) { slot = (int)slot; return C2_OK; }

    static c2_status_t getMinBuffersForDisplay(size_t* minBuffersForDisplay);

private:
    c2_status_t cancelAllBuffers();

    std::shared_ptr<android::C2AllocatorGralloc> mAllocator;
    const local_id_t mLocalId;

    // Function mutex to lock at the start of each API function call for protecting the
    // synchronization of all member variables.
    std::mutex mMutex;
    // The ids of all allocated buffers.
    std::set<uint32_t> mBufferIds GUARDED_BY(mMutex);
    // The maximum count of allocated buffers.
    size_t mBufferCount GUARDED_BY(mMutex){0};
    // The timestamp for the next fetchGraphicBlock() call.
    // Set when the previous fetchGraphicBlock() call timed out.
    int64_t mNextFetchTimeUs GUARDED_BY(mMutex){0};
};

#endif  // ANDROID_C2_VDA_POOLED_BLOCK_POOL_H_
