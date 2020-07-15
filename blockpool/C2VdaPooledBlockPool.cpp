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

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VdaPooledBlockPool"

#include <errno.h>

#include <mutex>

#include <utils/Log.h>

#include <C2AllocatorGralloc.h>
#include <C2BlockInternal.h>
#include <C2PlatformSupport.h>
#include <utils/CallStack.h>

#include <hidl/HidlSupport.h>
#include <ui/BufferQueueDefs.h>
#include <ui/GraphicBuffer.h>
#include <ui/Fence.h>

#include <types.h>
#include <utils/CallStack.h>
#include <C2VDASupport.h>
#include <hardware/gralloc1.h>
#include "C2VdaPooledBlockPool.h"

using ::android::C2AllocatorGralloc;
using ::android::C2AndroidMemoryUsage;
using ::android::Fence;
using ::android::GraphicBuffer;
using ::android::sp;
using ::android::status_t;
using ::android::wp;
using ::android::hardware::hidl_handle;
using ::android::hardware::Return;

using ::android::hardware::graphics::common::V1_0::PixelFormat;

using namespace android;

C2VdaPooledBlockPool::C2VdaPooledBlockPool(std::shared_ptr<C2AllocatorGralloc> allocator,
                                   const local_id_t localId)
      : C2PooledBlockPool(allocator, localId),
        mAllocator(allocator),
        mLocalId(localId) {
    ALOGI("C2VdaPooledBlockPool mAllocator.use_count() %ld\n", mAllocator.use_count());
}

C2VdaPooledBlockPool::~C2VdaPooledBlockPool() {
    ALOGI("~C2VdaPooledBlockPool this %p, mAllocator.use_count() %ld\n", this, mAllocator.use_count());
    //std::lock_guard<std::mutex> lock(mMutex);
    cancelAllBuffers();
    mAllocator.reset();
}

c2_status_t C2VdaPooledBlockPool::fetchGraphicBlock(
        uint32_t width, uint32_t height, uint32_t format, C2MemoryUsage usage,
        std::shared_ptr<C2GraphicBlock>* block /* nonnull */) {

    ALOGI("C2VdaPooledBlockPool::fetchGraphicBlock\n");
    //std::lock_guard<std::mutex> lock(mMutex);
    C2PooledBlockPool::fetchGraphicBlock(width, height, format, usage, block);

    mBlockAllocations[block->get()] = mBufferCountCurrent;
    ALOGI("RRRRRRRRRRR fetchGraphicBlock mBufferCountCurrent %d, mBufferCountTotal %d\n",
        mBufferCountCurrent, mBufferCountTotal);
    mBufferCountCurrent = (mBufferCountCurrent+1) % mBufferCountTotal;

    return C2_OK;
}

c2_status_t C2VdaPooledBlockPool::requestNewBufferSet(int32_t bufferCount) {
    ALOGI("requestNewBufferSet max buffer count %d\n", bufferCount);
    //std::lock_guard<std::mutex> lock(mMutex);
    if (bufferCount <= 0) {
        ALOGE("Invalid requested buffer count = %d", bufferCount);
        return C2_BAD_VALUE;
    }

    mBufferCountTotal = bufferCount;
    mBufferCountCurrent = 0;
    return C2_OK;
}

c2_status_t C2VdaPooledBlockPool::cancelAllBuffers() {
    ALOGI("cancelAllBuffers mAllocator.use_count() %ld\n", mAllocator.use_count());
    mBlockAllocations.clear();
    ALOGI("cancelAllBuffers mBlockAllocations %d out\n", mBlockAllocations.size());
    return C2_OK;
}

c2_status_t C2VdaPooledBlockPool::getPoolIdFromGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t* poolId) {
    auto iter = mBlockAllocations.find(block.get());
    ALOGI("android::BlockAllocations poolid %d, block %p\n", mBlockAllocations[block.get()], block.get());
    if (iter == mBlockAllocations.end())
        return C2_BAD_VALUE;
    *poolId = mBlockAllocations[block.get()];
    mBlockAllocations.erase(block.get());
    return C2_OK;
}

c2_status_t C2VdaPooledBlockPool::getMinBuffersForDisplay(size_t* minBuffersForDisplay) {
        ALOGI("getMinBuffersForDisplay\n");
    *minBuffersForDisplay = 6;
    return C2_OK;
}

