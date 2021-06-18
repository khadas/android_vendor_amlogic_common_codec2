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
#define LOG_TAG "C2VdaBqBlockPool"

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
#include "C2VdaBqBlockPool.h"

using ::android::BufferQueueDefs::NUM_BUFFER_SLOTS;
using ::android::C2AllocatorGralloc;
using ::android::C2AndroidMemoryUsage;
using ::android::Fence;
using ::android::GraphicBuffer;
using ::android::sp;
using ::android::status_t;
using ::android::wp;
using ::android::hardware::hidl_handle;
using ::android::hardware::Return;

using HBuffer = ::android::hardware::graphics::common::V1_2::HardwareBuffer;
using HStatus = ::android::hardware::graphics::bufferqueue::V2_0::Status;
using ::android::hardware::graphics::bufferqueue::V2_0::utils::b2h;
using ::android::hardware::graphics::bufferqueue::V2_0::utils::h2b;
using ::android::hardware::graphics::bufferqueue::V2_0::utils::HFenceWrapper;

using HGraphicBufferProducer = ::android::hardware::graphics::bufferqueue::V2_0
        ::IGraphicBufferProducer;
using ::android::hardware::graphics::common::V1_0::PixelFormat;

using namespace android;

namespace {

// The wait time for acquire fence in milliseconds.
const int kFenceWaitTimeMs = 10;
// The timeout delay for dequeuing buffer from producer in nanoseconds.
const int64_t kDequeueTimeoutNs = 10 * 1000 * 1000;

}  // namespace

static std::shared_ptr<C2AllocatorGralloc> sAllocatorDummy = NULL;

static c2_status_t asC2Error(int32_t err) {
    switch (err) {
    case android::NO_ERROR:
        return C2_OK;
    case android::NO_INIT:
        return C2_NO_INIT;
    case android::BAD_VALUE:
        return C2_BAD_VALUE;
    case android::TIMED_OUT:
        return C2_TIMED_OUT;
    case android::WOULD_BLOCK:
        return C2_BLOCKING;
    case android::NO_MEMORY:
        return C2_NO_MEMORY;
    case -ETIME:
        return C2_TIMED_OUT;  // for fence wait
    }
    return C2_CORRUPTED;
}

C2VdaBqBlockPool::C2VdaBqBlockPool(std::shared_ptr<C2AllocatorGralloc> allocator,
                                   const local_id_t localId)
      : C2BufferQueueBlockPool(sAllocatorDummy, localId),
        mAllocator(allocator),
        mLocalId(localId),
        mMaxDequeuedBuffers(0u),
        mConsumerUsage(0) {
    ALOGI("C2VdaBqBlockPool mAllocator.use_count() %ld\n", mAllocator.use_count());
}

C2VdaBqBlockPool::~C2VdaBqBlockPool() {
    ALOGI("~C2VdaBqBlockPool this %p, mAllocator.use_count() %ld\n", this, mAllocator.use_count());
    std::lock_guard<std::mutex> lock(mMutex);
    cancelAllBuffers();
    mAllocator.reset();
}

c2_status_t C2VdaBqBlockPool::fetchGraphicBlock(
        uint32_t width, uint32_t height, uint32_t format, C2MemoryUsage usage,
        std::shared_ptr<C2GraphicBlock>* block /* nonnull */) {

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mProducer) {
        ALOGI("no producer, fetch error\n");
        return C2_NO_INIT;
    }

    if (mSlotAllocations.size() < mMaxDequeuedBuffers) {
        Return<HStatus> transResult = mProducer->allowAllocation(true);
        if (!transResult.isOk()) {
            ALOGE("allowAllocation(false) failed");
            return C2_BAD_VALUE;
        }
    }

    sp<Fence> fence = new Fence();
    C2AndroidMemoryUsage androidUsage = usage;
    int32_t status;
    int32_t slot;
    bool bufferNeedsReallocation{};

    //ALOGI("fetchGraphicBlock dequeueBuffer [%d x %d] fence %p\n", width, height, fence.get());
    using Input = HGraphicBufferProducer::DequeueBufferInput;
    using Output = HGraphicBufferProducer::DequeueBufferOutput;
    Return<void> transResult = mProducer->dequeueBuffer(
            Input{
                width,
                height,
                format,
                androidUsage.asGrallocUsage()},
            [&status, &slot, &bufferNeedsReallocation,
             &fence](HStatus hStatus,
                     int32_t hSlot,
                     Output const& hOutput) {
                slot = static_cast<int>(hSlot);
                if (!h2b(hStatus, &status) ||
                        !h2b(hOutput.fence, &fence)) {
                    status = ::android::BAD_VALUE;
                } else {
                    bufferNeedsReallocation =
                            hOutput.bufferNeedsReallocation;
                }
            });
    ALOGV("dequeueBuffer slot:%d, and wait fence", slot);
    // check dequeueBuffer return flag
    if (!transResult.isOk() || status != android::OK) {
        if (transResult.isOk()) {
            if (status == android::INVALID_OPERATION ||
                status == android::TIMED_OUT ||
                status == android::WOULD_BLOCK) {
                ALOGD("dequeue timeout %d", status);
                return C2_TIMED_OUT;
            }
        }
        ALOGD("cannot dequeue buffer %d", status);
        return C2_BAD_VALUE;
    }

    // wait for acquire fence if we get one.
    HFenceWrapper hFenceWrapper{};
    if (!b2h(fence, &hFenceWrapper)) {
        ALOGE("Invalid fence received from dequeueBuffer.");
        return C2_BAD_VALUE;
    }
    if (fence) {
        status_t status = fence->wait(kFenceWaitTimeMs);
        if (status == -ETIME) {
            // fence is not signalled yet.
            (void)mProducer->cancelBuffer(slot, hFenceWrapper.getHandle()).isOk();
            ALOGV("fence wait timeout");
            return C2_TIMED_OUT;
        }
        if (status != android::NO_ERROR) {
            ALOGD("buffer fence wait error %d", status);
            (void)mProducer->cancelBuffer(slot, hFenceWrapper.getHandle()).isOk();
            return C2_BAD_VALUE;
        }
    }
    ALOGI("%s dequeued slot %d successfully", __func__, slot);

    auto iter = mSlotAllocations.find(slot);
    if ((iter == mSlotAllocations.end()) ||
        bufferNeedsReallocation) {
        // it's a new slot index, request for a new buffer.
        if (mSlotAllocations.size() >= mMaxDequeuedBuffers) {
            ALOGE("still get a new slot index but already allocated enough buffers.");
            (void)mProducer->cancelBuffer(slot, hFenceWrapper.getHandle()).isOk();
            return C2_CORRUPTED;
        }
        /*if (status != IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION) {
            ALOGE("expect BUFFER_NEEDS_REALLOCATION flag but didn't get one.");
            mProducer->cancelBuffer(slot, hFenceWrapper.getHandle());
            return C2_CORRUPTED;
        }*/

        sp<GraphicBuffer> slotBuffer = new GraphicBuffer();

        // N.B. This assumes requestBuffer# returns an existing allocation
        // instead of a new allocation.
        Return<void> transResult = mProducer->requestBuffer(
                slot,
                [&status, &slotBuffer](
                        HStatus hStatus,
                        HBuffer const& hBuffer,
                        uint32_t generationNumber){
                    if (h2b(hStatus, &status) &&
                            h2b(hBuffer, &slotBuffer) &&
                            slotBuffer) {
                        slotBuffer->setGenerationNumber(generationNumber);
                    } else {
                        status = android::BAD_VALUE;
                    }
                });
        // check requestBuffer return flag
        if (!transResult.isOk()) {
            ALOGI("requestBuffer C2_BAD_VALUE");
                return C2_BAD_VALUE;
        } else if (status != android::NO_ERROR) {
            ALOGE("requestBuffer failed: %d", status);
            mProducer->cancelBuffer(slot, hFenceWrapper.getHandle());
            return asC2Error(status);
        }

        // convert GraphicBuffer to C2GraphicAllocation and wrap producer id and slot index
        ALOGI("buffer wraps { producer id: %" PRIu64 ", slot: %d }", mProducerId, slot);
        C2Handle *c2Handle = android::WrapNativeCodec2GrallocHandle(
                slotBuffer->handle,
                slotBuffer->width,
                slotBuffer->height,
                slotBuffer->format,
                slotBuffer->usage,
                slotBuffer->stride,
                slotBuffer->getGenerationNumber(),
                mProducerId, slot);
        ALOGI("slotBuffer->handle %d, c2Handle %d\n", slotBuffer->handle->data[0], c2Handle->data[0]);
        if (!c2Handle) {
            ALOGE("WrapNativeCodec2GrallocHandle failed");
            return C2_NO_MEMORY;
        }

        std::shared_ptr<C2GraphicAllocation> alloc;
        c2_status_t err = mAllocator->priorGraphicAllocation(c2Handle, &alloc);
        if (err != C2_OK) {
            ALOGE("priorGraphicAllocation failed: %d", err);
            return err;
        }

        mSlotAllocations[slot] = std::move(alloc);
        ALOGI("mSlotAllocations.size() %d, \n", mSlotAllocations.size());
        if (mSlotAllocations.size() == mMaxDequeuedBuffers) {
            // already allocated enough buffers, set allowAllocation to false to restrict the
            // eligible slots to allocated ones for future dequeue.
            Return<HStatus> transResult = mProducer->allowAllocation(false);
            if (!transResult.isOk()) {
                ALOGE("allowAllocation(false) failed");
                return asC2Error(status);
            }
        }
    } else if (mSlotAllocations.size() < mMaxDequeuedBuffers) {
        ALOGE("failed to allocate enough buffers");
        return C2_BAD_STATE;
    }

    *block = _C2BlockFactory::CreateGraphicBlock(mSlotAllocations[slot]);
    mBlockAllocations[block->get()] = slot;
    ALOGV("fetchGraphicBlock slot %d, block %p\n", slot, block->get());
    return C2_OK;
}

c2_status_t C2VdaBqBlockPool::requestNewBufferSet(int32_t bufferCount) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (bufferCount <= 0) {
        ALOGE("Invalid requested buffer count = %d", bufferCount);
        return C2_BAD_VALUE;
    }

    ALOGI("requestNewBufferSet\n");
    if (!mProducer) {
        ALOGD("No HGraphicBufferProducer is configured...");
        return C2_NO_INIT;
    }
#if 0
    //no producer switch
    {
        sp<Fence> fence(Fence::NO_FENCE);
        HFenceWrapper hFenceWrapper{};
        b2h(fence, &hFenceWrapper);
        for (int i = 0; i < NUM_BUFFER_SLOTS; ++i) {
            Return<HStatus> transResult =
                    mProducer->detachBuffer(static_cast<int32_t>(i));
            transResult = mProducer->cancelBuffer(i, hFenceWrapper.getHandle());
        }
    }
#endif

    // TODO: should we query(NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS) and add it on?
    Return<HStatus> transResult = mProducer->setMaxDequeuedBufferCount(bufferCount + 8 /*mSlotAllocations.size()*/);
    if (!transResult.isOk()) {
        ALOGE("setMaxDequeuedBufferCount failed");
        return C2_BAD_VALUE;
    }
    mMaxDequeuedBuffers = static_cast<size_t>(bufferCount);
    mSlotAllocations.clear();

    transResult = mProducer->allowAllocation(true);
    if (!transResult.isOk()) {
        ALOGE("allowAllocation(true) failed");
        return C2_BAD_VALUE;
    }
    return C2_OK;
}

void C2VdaBqBlockPool::configureProducer(const sp<HGraphicBufferProducer>& producer) {
    // TODO: handle producer change request (client changes surface) while codec is running.
    ALOGI(">>>>>>configureProducer producer %p\n", producer.get());
    std::lock_guard<std::mutex> lock(mMutex);
    if (mProducer) {
        sp<Fence> fence(Fence::NO_FENCE);
        HFenceWrapper hFenceWrapper{};
        b2h(fence, &hFenceWrapper);
        for (int i = 0; i < NUM_BUFFER_SLOTS; ++i) {
            Return<HStatus> transResult =
                    mProducer->detachBuffer(static_cast<int32_t>(i));
            transResult = mProducer->cancelBuffer(i, hFenceWrapper.getHandle());
        }
        mProducer = nullptr;
        mAllocator.reset();
        mSlotAllocations.clear();
        mBlockAllocations.clear();
        ALOGI(">>>>>>configureProducer to null\n");
        return;
    }

    mProducer = producer;
    if (producer) {
        ALOGI("configureProducer mProducer %p\n", mProducer.get());
        Return<uint64_t> transResult = producer->getUniqueId();
        if (!transResult.isOk()) {
            ALOGE("getUniqueId failed");
            mProducer = nullptr;
            mProducerId = 0;
            return;
        }
        mProducerId = static_cast<uint64_t>(transResult);
        Return<HStatus> transResult2 = producer->setDequeueTimeout(kDequeueTimeoutNs);
        if (!transResult2.isOk()) {
            ALOGE("setDequeueTimeout failed");
            mProducer = nullptr;
            mProducerId = 0;
            return;
        }
        Return<HStatus> transResult3 = mProducer->allowAllocation(false);
        if (!transResult3.isOk()) {
            ALOGE("allowAllocation(false) failed");
            return ;
        }
    } else {
        ALOGI("configureProducer nullptr\n");
        mProducer = nullptr;
        mProducerId = 0;
    }
}
void C2VdaBqBlockPool::configureProducer(
            const android::sp<HGraphicBufferProducer> &producer,
            native_handle_t *syncMemory,
            uint64_t bqId,
            uint32_t generationId,
            uint64_t consumerUsage) {
    (void) syncMemory;
    (void) bqId;
    (void) generationId;
    ALOGI("configureProducer consumerUsage:%llx\n", consumerUsage);
    mConsumerUsage = consumerUsage;
    configureProducer(producer);
}


c2_status_t C2VdaBqBlockPool::cancelAllBuffers() {
    ALOGI("cancelAllBuffers mAllocator.use_count() %ld\n", mAllocator.use_count());
    mSlotAllocations.clear();
    mBlockAllocations.clear();
    ALOGI("cancelAllBuffers mSlotAllocations.size() %d, mBlockAllocations %d out\n",
                mSlotAllocations.size(), mBlockAllocations.size());
    return C2_OK;
}

c2_status_t C2VdaBqBlockPool::getPoolIdFromGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t* poolId) {
    auto iter = mBlockAllocations.find(block.get());
    ALOGV("android::BlockAllocations poolid %d, block %p\n", mBlockAllocations[block.get()], block.get());
    if (iter == mBlockAllocations.end())
        return C2_BAD_VALUE;
    *poolId = mBlockAllocations[block.get()];
    mBlockAllocations.erase(block.get());
    return C2_OK;
}

c2_status_t C2VdaBqBlockPool::resetGraphicBlock(int32_t slot) {
    std::lock_guard<std::mutex> lock(mMutex);
#if 0
    if (mProducer && slot < NUM_BUFFER_SLOTS) {
        sp<Fence> fence(Fence::NO_FENCE);
        HFenceWrapper hFenceWrapper{};
        b2h(fence, &hFenceWrapper);
        Return<HStatus> transResult =
            mProducer->detachBuffer(static_cast<int32_t>(slot));
        transResult = mProducer->cancelBuffer(slot, hFenceWrapper.getHandle());
    }
    auto iter = mSlotAllocations.find(slot);
    if (iter != mSlotAllocations.end()) {
        const C2Handle *chandle = iter->second->handle();
        ALOGI("slot alloctaion close slot:%d, fd=%d", slot, chandle->data[0]);
        {
            sp<Fence> fence(Fence::NO_FENCE);
            HFenceWrapper hFenceWrapper{};
            b2h(fence, &hFenceWrapper);
            Return<HStatus> transResult =
                mProducer->detachBuffer(static_cast<int32_t>(slot));
            transResult = mProducer->cancelBuffer(slot, hFenceWrapper.getHandle());
        }
        iter->second.reset();
        mSlotAllocations.erase(iter);
    }
    if (mSlotAllocations.size() < mMaxDequeuedBuffers) {
        Return<HStatus> transResult = mProducer->allowAllocation(true);
        if (!transResult.isOk()) {
            ALOGE("allowAllocation(false) failed");
            return C2_BAD_VALUE;
        }
    }
#endif
    slot = (int)slot;
    return C2_OK;
}

int64_t C2VdaBqBlockPool::getSurfaceUsage() {
    if (!mProducer) {
        ALOGI("no producer, fetch error\n");
        return 0;
    }
    if (mConsumerUsage != 0) {
        ALOGI("getSurfaceUsage, mConsumerUsage:%llx\n", mConsumerUsage);
        return mConsumerUsage;
    }

    uint32_t width = 64;
    uint32_t height = 64;
    uint32_t format = 0x11;//HalPixelFormat::YCRCB_420_SP;
    C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
    sp<Fence> fence = new Fence();
    C2AndroidMemoryUsage androidUsage = usage;
    int32_t status;
    int32_t slot;
    bool bufferNeedsReallocation{};

    {
        Return<HStatus> transResult = mProducer->allowAllocation(true);
        if (!transResult.isOk()) {
            ALOGE("allowAllocation(true) failed");
            return 0;
        }
    }

    //ALOGI("fetchGraphicBlock dequeueBuffer [%d x %d] fence %p\n", width, height, fence.get());
    using Input = HGraphicBufferProducer::DequeueBufferInput;
    using Output = HGraphicBufferProducer::DequeueBufferOutput;
    Return<void> transResult = mProducer->dequeueBuffer(
            Input{
                width,
                height,
                format,
                androidUsage.asGrallocUsage()},
            [&status, &slot, &bufferNeedsReallocation,
             &fence](HStatus hStatus,
                     int32_t hSlot,
                     Output const& hOutput) {
                slot = static_cast<int>(hSlot);
                if (!h2b(hStatus, &status) ||
                        !h2b(hOutput.fence, &fence)) {
                    status = ::android::BAD_VALUE;
                } else {
                    bufferNeedsReallocation =
                            hOutput.bufferNeedsReallocation;
                }
            });
    ALOGV("dequeueBuffer slot:%d, and wait fence", slot);
    // check dequeueBuffer return flag
    if (!transResult.isOk() || status != android::OK) {
        if (transResult.isOk()) {
            if (status == android::INVALID_OPERATION ||
                status == android::TIMED_OUT ||
                status == android::WOULD_BLOCK) {
                ALOGD("dequeue timeout %d", status);
                return 0;
            }
        }
        ALOGD("cannot dequeue buffer %d", status);
        return 0;
    }

    // wait for acquire fence if we get one.
    HFenceWrapper hFenceWrapper{};
    if (!b2h(fence, &hFenceWrapper)) {
        ALOGE("Invalid fence received from dequeueBuffer.");
        return 0;
    }
    if (fence) {
        status_t status = fence->wait(kFenceWaitTimeMs);
        if (status == -ETIME) {
            // fence is not signalled yet.
            (void)mProducer->cancelBuffer(slot, hFenceWrapper.getHandle()).isOk();
            ALOGV("fence wait timeout");
            return 0;
        }
        if (status != android::NO_ERROR) {
            ALOGD("buffer fence wait error %d", status);
            (void)mProducer->cancelBuffer(slot, hFenceWrapper.getHandle()).isOk();
            return 0;
        }
    }

    sp<GraphicBuffer> slotBuffer = new GraphicBuffer();
    transResult = mProducer->requestBuffer(
        slot,
        [&status, &slotBuffer](
                HStatus hStatus,
                HBuffer const& hBuffer,
                uint32_t generationNumber){
            if (h2b(hStatus, &status) &&
                    h2b(hBuffer, &slotBuffer) &&
                    slotBuffer) {
                slotBuffer->setGenerationNumber(generationNumber);
            } else {
                status = android::BAD_VALUE;
            }
    });
    // check requestBuffer return flag
    if (!transResult.isOk()) {
        ALOGI("requestBuffer C2_BAD_VALUE");
        return 0;
    } else if (status != android::NO_ERROR) {
        ALOGE("requestBuffer failed: %d", status);
        mProducer->cancelBuffer(slot, hFenceWrapper.getHandle());
        return 0;
    }

    {
        sp<Fence> fence(Fence::NO_FENCE);
        HFenceWrapper hFenceWrapper{};
        b2h(fence, &hFenceWrapper);
        Return<HStatus> transResult =
        mProducer->detachBuffer(static_cast<int32_t>(slot));
        transResult = mProducer->cancelBuffer(slot, hFenceWrapper.getHandle());
        if (status != android::NO_ERROR) {
            ALOGI("detach fails");
            return 0;
        }
    }

    {
        Return<HStatus> transResult = mProducer->allowAllocation(false);
        if (!transResult.isOk()) {
            ALOGE("allowAllocation(false) failed");
            return 0;
        }
    }

    return slotBuffer->getUsage();
}

c2_status_t C2VdaBqBlockPool::getMinBuffersForDisplay(size_t* minBuffersForDisplay) {
    ALOGV("getMinBuffersForDisplay\n");
    *minBuffersForDisplay = 6;
    return C2_OK;
}

