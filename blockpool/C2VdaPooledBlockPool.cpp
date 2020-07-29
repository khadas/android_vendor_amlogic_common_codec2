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

#include <bufferpool/BufferPoolTypes.h>

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
using android::hardware::media::bufferpool::BufferPoolData;

using namespace android;

// The wait time for another try to fetch a buffer from bufferpool.
const int64_t kFetchRetryDelayUs = 10 * 1000;

int64_t GetNowUs() {
    struct timespec t;
    t.tv_sec = 0;
    t.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t nsecs = static_cast<int64_t>(t.tv_sec) * 1000000000LL + t.tv_nsec;
    return nsecs / 1000ll;
}

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
    ALOGV("C2VdaPooledBlockPool::fetchGraphicBlock\n");
    ALOG_ASSERT(block != nullptr);
    uint32_t poolId = 0;
    std::lock_guard<std::mutex> lock(mMutex);

    if (mNextFetchTimeUs != 0) {
        int delayUs = GetNowUs() - mNextFetchTimeUs;
        if (delayUs > 0) {
            ::usleep(delayUs);
        }
        mNextFetchTimeUs = 0;
    }

    std::shared_ptr<C2GraphicBlock> fetchBlock;
    c2_status_t err =
        C2PooledBlockPool::fetchGraphicBlock(width, height, format, usage, &fetchBlock);
    if (err != C2_OK) {
        ALOGE("Failed at C2PooledBlockPool::fetchGraphicBlock: %d", err);
        return err;
    }

    err = getPoolIdFromGraphicBlock(fetchBlock, &poolId);
    if (err != C2_OK) {
        ALOGE("Failed to getPoolIdFromGraphicBlock");
        return C2_CORRUPTED;
    }

    if (mBufferIds.size() < mBufferCount) {
        mBufferIds.insert(poolId);
    }

    if (mBufferIds.find(poolId) != mBufferIds.end()) {
        ALOGV("Returned buffer id = %u", poolId);
        *block = std::move(fetchBlock);
        return C2_OK;
    }
    ALOGV("No buffer could be recycled now, wait for another try...");
    mNextFetchTimeUs = GetNowUs() + kFetchRetryDelayUs;
    return C2_TIMED_OUT;
}

c2_status_t C2VdaPooledBlockPool::requestNewBufferSet(int32_t bufferCount) {
    if (bufferCount <= 0) {
        ALOGE("Invalid requested buffer count = %d", bufferCount);
        return C2_BAD_VALUE;
    }

    ALOGI("requestNewBufferSet max buffer count %d\n", bufferCount);
    std::lock_guard<std::mutex> lock(mMutex);
    mBufferIds.clear();
    mBufferCount = bufferCount;
    return C2_OK;
}

c2_status_t C2VdaPooledBlockPool::cancelAllBuffers() {
    ALOGI("cancelAllBuffers mAllocator.use_count() %ld\n", mAllocator.use_count());
    mBufferIds.clear();
    mBufferCount = 0;
    return C2_OK;
}

c2_status_t C2VdaPooledBlockPool::getPoolIdFromGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t* poolId) {
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
    *poolId = bpData->mId;
    return C2_OK;
}

c2_status_t C2VdaPooledBlockPool::getMinBuffersForDisplay(size_t* minBuffersForDisplay) {
        ALOGI("getMinBuffersForDisplay\n");
    *minBuffersForDisplay = 6;
    return C2_OK;
}

