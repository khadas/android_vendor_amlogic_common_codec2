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
#include <sys/stat.h>

#include <C2AllocatorGralloc.h>
#include <C2BlockInternal.h>
#include <C2PlatformSupport.h>
#include <C2SurfaceSyncObj.h>

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
// Use 1000ms as some as omx and videotunnel.
const int kFenceWaitTimeMs = 1000;
// The timeout delay for dequeuing buffer from producer in nanoseconds.
const int64_t kDequeueTimeoutNs = 10 * 1000 * 1000;

bool getGenerationNumberAndUsage(const sp<HGraphicBufferProducer> &producer,
                                 uint32_t *generation, uint64_t *usage) {
    status_t status{};
    int slot{};
    bool bufferNeedsReallocation{};
    sp<Fence> fence = new Fence();

    using Input = HGraphicBufferProducer::DequeueBufferInput;
    using Output = HGraphicBufferProducer::DequeueBufferOutput;
    Return<void> transResult = producer->dequeueBuffer(
            Input{640, 480, HAL_PIXEL_FORMAT_YCBCR_420_888, 0},
            [&status, &slot, &bufferNeedsReallocation, &fence]
            (HStatus hStatus, int32_t hSlot, Output const& hOutput) {
                slot = static_cast<int>(hSlot);
                if (!h2b(hStatus, &status) || !h2b(hOutput.fence, &fence)) {
                    status = ::android::BAD_VALUE;
                } else {
                    bufferNeedsReallocation =
                            hOutput.bufferNeedsReallocation;
                }
            });
    if (!transResult.isOk() || status != android::OK) {
        return false;
    }
    HFenceWrapper hFenceWrapper{};
    if (!b2h(fence, &hFenceWrapper)) {
        (void)producer->detachBuffer(static_cast<int32_t>(slot)).isOk();
        ALOGE("Invalid fence received from dequeueBuffer.");
        return false;
    }
    sp<GraphicBuffer> slotBuffer = new GraphicBuffer();
    // N.B. This assumes requestBuffer# returns an existing allocation
    // instead of a new allocation.
    transResult = producer->requestBuffer(
            slot,
            [&status, &slotBuffer, &generation, &usage](
                    HStatus hStatus,
                    HBuffer const& hBuffer,
                    uint32_t generationNumber){
                if (h2b(hStatus, &status) &&
                        h2b(hBuffer, &slotBuffer) &&
                        slotBuffer) {
                    *generation = generationNumber;
                    *usage = slotBuffer->getUsage();
                    slotBuffer->setGenerationNumber(generationNumber);
                } else {
                    status = android::BAD_VALUE;
                }
            });
    if (!transResult.isOk()) {
        return false;
    } else if (status != android::NO_ERROR) {
        (void)producer->detachBuffer(static_cast<int32_t>(slot)).isOk();
        return false;
    }
    (void)producer->detachBuffer(static_cast<int32_t>(slot)).isOk();
    return true;
}


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
    //ALOGI("mSlotAllocations:%d, mOriginalSlotInode:%d,mMaxDequeuedBuffers:%d\n", mSlotAllocations.size(), mOriginalSlotInode.size(), mMaxDequeuedBuffers);
    migrateLostBuf();

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
    ALOGV("dequeueBuffer slot:%d, and wait fence", slot);

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

    ALOGI("%s dequeued slot %d successfully, bufferNeedsReallocation:%d", __func__, slot, bufferNeedsReallocation);
    sp<GraphicBuffer> &slotBuffer = mBuffers[slot];

    auto iter = mSlotAllocations.find(slot);
    if ((iter == mSlotAllocations.end()) ||
        bufferNeedsReallocation) {
        // it's a new slot index, request for a new buffer.
        if (!bufferNeedsReallocation && (mSlotAllocations.size() >= mMaxDequeuedBuffers || slot >= mMaxDequeuedBuffers)) {
            if (mProducer->cancelBuffer(slot, hFenceWrapper.getHandle()).isOk())
                return C2_TIMED_OUT;
            ALOGE("still get a new slot index but already allocated enough buffers.");
            return C2_CORRUPTED;
        }

        if (!slotBuffer) {
            slotBuffer = new GraphicBuffer();
        }
        uint32_t outGeneration;

        // N.B. This assumes requestBuffer# returns an existing allocation
        // instead of a new allocation.
        Return<void> transResult = mProducer->requestBuffer(
                slot,
                [&status, &slotBuffer, &outGeneration](
                        HStatus hStatus,
                        HBuffer const& hBuffer,
                        uint32_t generationNumber){
                    if (h2b(hStatus, &status) &&
                            h2b(hBuffer, &slotBuffer) &&
                            slotBuffer) {
                        slotBuffer->setGenerationNumber(generationNumber);
                        outGeneration = generationNumber;
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
        if (mGeneration == 0) {
            // getting generation # lazily due to dequeue failure.
            mGeneration = outGeneration;
        }
        if (slotBuffer) {
            if (bufferNeedsReallocation) {
                uint64_t inode;
                getINodeFromFd(slotBuffer->handle->data[0], &inode);
                ALOGV("set original slot : %d, inode :%lld", slot, inode);
                mOriginalSlotInode[slot] = inode;
            } else if (mOriginalSlotInode.size() < mMaxDequeuedBuffers) {
                auto iter = mOriginalSlotInode.find(slot);
                if (iter == mOriginalSlotInode.end()) {
                    uint64_t inode;
                    getINodeFromFd(slotBuffer->handle->data[0], &inode);
                    ALOGV("set original slot : %d, inode :%lld", slot, inode);
                    mOriginalSlotInode[slot] = inode;
                }
            }
        }
    }

    if (slotBuffer) {
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
            native_handle_close(c2Handle);
            native_handle_delete(c2Handle);
            return err;
        }

        mSlotAllocations[slot] = std::move(alloc);
        ALOGI("mSlotAllocations.size() %d, mMaxDequeuedBuffers:%d\n", mSlotAllocations.size(), mMaxDequeuedBuffers);
        if (mSlotAllocations.size() == mMaxDequeuedBuffers) {
            // already allocated enough buffers, set allowAllocation to false to restrict the
            // eligible slots to allocated ones for future dequeue.
            Return<HStatus> transResult = mProducer->allowAllocation(false);
            if (!transResult.isOk()) {
                ALOGE("allowAllocation(false) failed");
                return asC2Error(status);
            }
        }
        std::shared_ptr<C2BufferQueueBlockPoolData> poolData =
            std::make_shared<C2BufferQueueBlockPoolData>(
                            slotBuffer->getGenerationNumber(),
                            mProducerId, slot,
                            mProducer, mSyncMem, 0);
        mPoolDatas[slot] = poolData;

        *block = _C2BlockFactory::CreateGraphicBlock(mSlotAllocations[slot], poolData);
        mBlockAllocations[block->get()] = slot;
        setSlotToOriginal(slot, slotBuffer->handle->data[0]);

        ALOGV("fetchGraphicBlock slot %d, block %p, block fd: %d, c2Handle fd:%d\n", slot, block->get(), block->get()->handle()->data[0], c2Handle->data[0]);
    }


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

    Return<HStatus> transResult = mProducer->setMaxDequeuedBufferCount(bufferCount);
    if (!transResult.isOk()) {
        ALOGE("setMaxDequeuedBufferCount failed");
        return C2_BAD_VALUE;
    }
    mMaxDequeuedBuffers = static_cast<size_t>(bufferCount);
    mSlotAllocations.clear();
    mOriginalSlotInode.clear();
    mSlotToOriginal.clear();

    transResult = mProducer->allowAllocation(true);
    if (!transResult.isOk()) {
        ALOGE("allowAllocation(true) failed");
        return C2_BAD_VALUE;
    }
    return C2_OK;
}


/* This is for Old HAL request for compatibility */
void C2VdaBqBlockPool::configureProducer(const sp<HGraphicBufferProducer> &producer) {
    ALOGV("configureProducer");
    uint64_t producerId = 0;
    uint32_t generation = 0;
    uint64_t usage = 0;
    bool bqInformation = false;
    if (producer) {
        Return<uint64_t> transResult = producer->getUniqueId();
        if (!transResult.isOk()) {
            ALOGD("configureProducer -- failed to connect to the producer");
            return;
        }
        producerId = static_cast<uint64_t>(transResult);
        bqInformation = getGenerationNumberAndUsage(producer, &generation, &usage);
        if (!bqInformation) {
            ALOGW("get generationNumber failed %llu",
                  (unsigned long long)producerId);
        }
    }
    configureProducer(producer, nullptr, producerId, generation, usage);
}
void C2VdaBqBlockPool::configureProducer(const sp<HGraphicBufferProducer> &producer,
                       native_handle_t *syncHandle,
                       uint64_t producerId,
                       uint32_t generation,
                       uint64_t usage) {
    ALOGD("configureProducer, producerId:%lld, generation:%d, usage:%llx" , producerId, generation, usage);

    if (((mConsumerUsage & GRALLOC_USAGE_HW_COMPOSER) != 0) &&  (usage & GRALLOC_USAGE_HW_COMPOSER) == 0) {
        ALOGD("mConsumerUsage:%llx, usage:%llx, surfaceview --> surfacetext do not change usage" , mConsumerUsage, usage);
    } else {
        mConsumerUsage = usage;
    }

    std::shared_ptr<C2SurfaceSyncMemory> c2SyncMem;
    if (syncHandle) {
        if (!producer) {
            native_handle_close(syncHandle);
            native_handle_delete(syncHandle);
        } else {
            c2SyncMem = C2SurfaceSyncMemory::Import(syncHandle);
        }
    }
    int migrated = 0;
    std::shared_ptr<C2SurfaceSyncMemory> oldMem;
    // poolDatas dtor should not be called during lock is held.
    std::shared_ptr<C2BufferQueueBlockPoolData>
            poolDatas[NUM_BUFFER_SLOTS];
    {
        sp<GraphicBuffer> buffers[NUM_BUFFER_SLOTS];
        std::scoped_lock<std::mutex> lock(mMutex);
        bool noInit = false;
        for (int i = 0; i < NUM_BUFFER_SLOTS; ++i) {
            if (!noInit && mProducer) {
                Return<HStatus> transResult =
                        mProducer->detachBuffer(static_cast<int32_t>(i));
                noInit = !transResult.isOk() ||
                         static_cast<HStatus>(transResult) == HStatus::NO_INIT;
            }
            mNeedMigrates[i] = false;
        }
        mOldProducerId = mGeneration;
        if (producer) {
            mProducer = producer;
            mProducerId = producerId;
            mGeneration = generation;
        } else {
            mProducer = nullptr;
            mProducerId = 0;
            mGeneration = 0;
            ALOGW("invalid producer producer(%d)",
                  (bool)producer);
        }
        oldMem = mSyncMem; // preven destruction while locked.
        mSyncMem = c2SyncMem;
        C2SyncVariables *syncVar = mSyncMem ? mSyncMem->mem() : nullptr;
        if (syncVar) {
            syncVar->lock();
            syncVar->setSyncStatusLocked(C2SyncVariables::STATUS_ACTIVE);
            syncVar->unlock();
        }
        std::map<int32_t, int32_t> oldSlotToOrg = mSlotToOriginal;
        if (mProducer) {

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
            mSlotToOriginal.clear();
        }
        if (mProducer) { // migrate buffers
            for (int i = 0; i < NUM_BUFFER_SLOTS; ++i) {
                std::shared_ptr<C2BufferQueueBlockPoolData> data =
                        mPoolDatas[i].lock();
                if (data) {
                    ALOGD("have data:%d", i);
                    int slot = data->migrate(
                            mProducer, mGeneration, mConsumerUsage,
                            mProducerId, mBuffers[i], mOldProducerId, mSyncMem);
                    if (slot >= 0) {
                        ALOGD("new slot: %d --> %d org:%d", i, slot, oldSlotToOrg[i]);
                        buffers[slot] = mBuffers[i];
                        poolDatas[slot] = data;
                        mSlotToOriginal[slot] = oldSlotToOrg[i] ;
                        ++migrated;
                    } else {
                        mNeedMigrates[i] = true;
                        ALOGD("no slot: %d", i);
                    }
                }
            }
        }
        for (int i = 0; i < NUM_BUFFER_SLOTS; ++i) {
            mBuffers[i] = buffers[i];
            mPoolDatas[i] = poolDatas[i];
        }
    }
    if (producer) {
        ALOGD("local generation change %u , "
              "bqId: %llu migrated buffers # %d",
              generation, (unsigned long long)producerId, migrated);
        mSlotAllocations.clear();
        mBlockAllocations.clear();
    }
}


c2_status_t C2VdaBqBlockPool::cancelAllBuffers() {
    ALOGI("cancelAllBuffers mAllocator.use_count() %ld\n", mAllocator.use_count());
    mSlotAllocations.clear();
    mBlockAllocations.clear();
    ALOGI("cancelAllBuffers mSlotAllocations.size() %d, mBlockAllocations %d out\n",
                mSlotAllocations.size(), mBlockAllocations.size());
    bool noInit = false;
    for (int i = 0; i < NUM_BUFFER_SLOTS; ++i) {
        if (!noInit && mProducer) {
            Return<HStatus> transResult =
                    mProducer->detachBuffer(static_cast<int32_t>(i));
            noInit = !transResult.isOk() ||
                     static_cast<HStatus>(transResult) == HStatus::NO_INIT;
        }
        mBuffers[i].clear();
    }
    return C2_OK;
}

c2_status_t C2VdaBqBlockPool::getPoolIdFromGraphicBlock(std::shared_ptr<C2GraphicBlock> block, uint32_t* poolId) {
    auto iter = mBlockAllocations.find(block.get());
    ALOGV("android::BlockAllocations poolid %d, block %p\n", mBlockAllocations[block.get()], block.get());
    if (iter == mBlockAllocations.end())
        return C2_BAD_VALUE;
    int32_t slot = mBlockAllocations[block.get()];
    *poolId = mSlotToOriginal[slot];
    ALOGV("slot:%d, original slot:%d", slot, *poolId );
    mBlockAllocations.erase(block.get());
    return C2_OK;
}

c2_status_t C2VdaBqBlockPool::resetGraphicBlock(int32_t originalSlot) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mProducer)
        return C2_OK;

    int32_t slot = -1;
    for (auto &entry : mSlotToOriginal) {
        if (entry.second == originalSlot) {
            slot = entry.first;
            break;
        }
    }

    if (slot == -1) {
        ALOGE("get slot failed");
        return C2_BAD_VALUE;
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

bool C2VdaBqBlockPool::getINodeFromFd(int32_t fd, uint64_t *ino) {
    ALOGV("getInode");
    struct stat st;
    int ret = fstat(fd, &st);
    if (ret == -1) {
        ALOGE("fstat error %d :%s", fd, strerror(errno));
        return false;
    } else {
        *ino = st.st_ino;
        ALOGV("[%d]==fstat: st_ino:%llu st_uid:%u st_gid:%u", fd, st.st_ino, st.st_uid, st.st_gid);
    }

    return true;
}


bool C2VdaBqBlockPool::migrateLostBuf() {
    std::shared_ptr<C2BufferQueueBlockPoolData>
        poolDatas[NUM_BUFFER_SLOTS];
    sp<GraphicBuffer> buffers[NUM_BUFFER_SLOTS];
    bool canSetBuffers = false;

    for (int i = 0; i < NUM_BUFFER_SLOTS; ++i) {
         if (mNeedMigrates[i] == true) {
             std::shared_ptr<C2BufferQueueBlockPoolData> data =
                         mPoolDatas[i].lock();
             if (data) {
                 ALOGD("have data:%d", i);
                 int slot = data->migrate(
                         mProducer, mGeneration, mConsumerUsage,
                         mProducerId, mBuffers[i], mOldProducerId, mSyncMem);
                 if (slot >= 0) {
                     ALOGD("new slot: %d --> %d", i, slot);
                     buffers[slot] = mBuffers[i];
                     poolDatas[slot] = data;
                     mNeedMigrates[i] = false;
                     canSetBuffers = true;
                 } else {
                     mNeedMigrates[i] = true;
                     ALOGD("no slot: %d", i);
                     canSetBuffers = false;
                 }
             }
         }

     }
     if (canSetBuffers) {
         for (int i = 0; i < NUM_BUFFER_SLOTS; ++i) {
             if (buffers[i].get() != NULL) {
                 mBuffers[i] = buffers[i];
                 mPoolDatas[i] = poolDatas[i];
             }
         }
     }
     return true;

}

bool C2VdaBqBlockPool::setSlotToOriginal(int32_t slot, int32_t fd) {
    auto iter = mSlotToOriginal.find(slot);
    if (iter == mSlotToOriginal.end()) {
        uint64_t inode;
        int32_t originalSlot = -1;
        getINodeFromFd(fd, &inode);

        for (auto &entry : mOriginalSlotInode) {
            if (entry.second == inode) {
                originalSlot = entry.first;
                break;
            }
        }
        if (originalSlot != -1) {
           ALOGV("set slot to original slot : %d, originalSlot :%d", slot, originalSlot);
           mSlotToOriginal[slot] = originalSlot;
           if (mNeedMigrates[slot] == true) {
                mNeedMigrates[slot] = false;
           }
        } else {
            ALOGE("can not get original slot");
            return false;
        }
        ALOGV("mSlotToOriginal size:%d", mSlotToOriginal.size());
#if 0//for debug
        for (auto &entry : mSlotToOriginal) {
            ALOGV("slot orslot [%d %d]",  entry.first, entry.second);
        }
#endif
    }
    return true;

}



