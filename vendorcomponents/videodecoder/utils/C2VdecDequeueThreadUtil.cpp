/*
 * Copyright (C) 2023 Amlogic, Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_NDEBUG 0
#define LOG_TAG "C2VdecDequeueThreadUtil"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <fstream>
#include <inttypes.h>
#include <string.h>
#include <algorithm>
#include <rect.h>
#include <size.h>

#include <base/bind.h>
#include <base/bind_helpers.h>
#include <C2VendorProperty.h>
#include <c2logdebug.h>
#include <C2VdecDebugUtil.h>
#include <C2VdecDeviceUtil.h>
#include <C2VdecInterfaceImpl.h>
#include <C2VdecDequeueThreadUtil.h>
#include <C2VdecBlockPoolUtil.h>

namespace android {

#define C2VdecDQ_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, comp->mCurInstanceID, C2VdecComponent::mInstanceNum, ##str)

#define DEFAULT_FRAME_DURATION (16384)// default dur: 16ms (1 frame at 60fps)
#define DEFAULT_START_OPTIMIZE_FRAME_NUMBER_MIN (100)

#define LockWeakPtrWithReturnVal(name, weak, retval) \
    auto name = weak.lock(); \
    if (name == nullptr) { \
        C2VdecDQ_LOG(CODEC2_LOG_ERR, "[%s:%d] null ptr, please check", __func__, __LINE__); \
        return retval;\
    }

#define LockWeakPtrWithReturnVoid(name, weak) \
    auto name = weak.lock(); \
    if (name == nullptr) { \
        C2VdecDQ_LOG(CODEC2_LOG_ERR, "[%s:%d] null ptr, please check", __func__, __LINE__); \
        return;\
    }

#define LockWeakPtrWithoutReturn(name, weak) \
    auto name = weak.lock(); \
    if (name == nullptr) { \
        C2VdecDQ_LOG(CODEC2_LOG_ERR, "[%s:%d] null ptr, please check", __func__, __LINE__); \
    }

C2VdecComponent::DequeueThreadUtil::DequeueThreadUtil() {
    mDequeueThread = new ::base::Thread("C2VdecDequeueThread");
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    CODEC2_LOG(CODEC2_LOG_INFO, "Creat DequeueThreadUtil!!");

    mRunTaskLoop.store(false);
    mAllocBufferLoop.store(false);

    mFetchBlockCount = 0;
    mStreamDurationUs = 0;
    mCurrentPixelFormat = 0;
    mMinFetchBlockInterval = 0;

    mLastAllocBufferRetryTimeUs = -1;
    mLastAllocBufferSuccessTimeUs = -1;
    memset(&mCurrentBlockSize, 0, sizeof(mCurrentBlockSize));
}

C2VdecComponent::DequeueThreadUtil::~DequeueThreadUtil() {
    CODEC2_LOG(CODEC2_LOG_INFO, "~DequeueThreadUtil!!");
    StopRunDequeueTask();
    if (mDequeueThread != NULL) {
        delete mDequeueThread;
        mDequeueThread = NULL;
    }
}

c2_status_t C2VdecComponent::DequeueThreadUtil::setComponent(std::shared_ptr<C2VdecComponent> sharecomp) {
    mComp = sharecomp;
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    mIntfImpl = comp->GetIntfImpl();

    C2VdecDQ_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
    return C2_OK;
}

bool C2VdecComponent::DequeueThreadUtil::StartRunDequeueTask(media::Size size, uint32_t pixelFormat) {
    LockWeakPtrWithReturnVal(comp, mComp, false);
    if (mRunTaskLoop.load() == true) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR,"[%s]dequeue thread is running, this is error!! return.", __func__);
        return false;
    }
    if (!mDequeueThread->Start()) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR, "[%s] Failed to start dequeue thread!!", __func__);
        return false;
    }
    mDequeueTaskRunner = mDequeueThread->task_runner();
    mRunTaskLoop.store(true);
    DCHECK(mDequeueTaskRunner != NULL);

    mDeviceUtil = comp->GetDeviceUtil();
    LockWeakPtrWithReturnVal(deviceUtil, mDeviceUtil, false);

    mStreamDurationUs = deviceUtil->getVideoDurationUs();
    mCurrentBlockSize = size;
    mCurrentPixelFormat = pixelFormat;
    mMinFetchBlockInterval = mStreamDurationUs / 4;
    C2VdecDQ_LOG(CODEC2_LOG_INFO,"%s task loop:%d alloc loop:%d duration:%d minfetchinterval:%d", __func__, mRunTaskLoop.load(), mAllocBufferLoop.load(), mStreamDurationUs, mMinFetchBlockInterval);
    return true;
}

void C2VdecComponent::DequeueThreadUtil::StopRunDequeueTask() {
    if (mDequeueThread->IsRunning()) {
        mRunTaskLoop.store(false);
        mDequeueThread->Stop();
    }
}

void C2VdecComponent::DequeueThreadUtil::StartAllocBuffer() {
    mAllocBufferLoop.store(true);
}

void C2VdecComponent::DequeueThreadUtil::StopAllocBuffer() {
    mAllocBufferLoop.store(false);
}

bool C2VdecComponent::DequeueThreadUtil::getAllocBufferLoopState() {
    return mAllocBufferLoop.load();
}

void C2VdecComponent::DequeueThreadUtil::postDelayedAllocTask(media::Size size, uint32_t pixelFormat, bool waitRunning, uint32_t delayTimeUs) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    if (!mRunTaskLoop.load()) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR,"postDequeueTask failed.");
        return;
    }

    if (waitRunning && !mAllocBufferLoop.load()) {
        mDequeueTaskRunner->PostDelayedTask(
                    FROM_HERE, ::base::Bind(&C2VdecComponent::DequeueThreadUtil::postDelayedAllocTask, ::base::Unretained(this),
                    size, pixelFormat, waitRunning, delayTimeUs), ::base::TimeDelta::FromMicroseconds(delayTimeUs));
        return;
    }

    mDequeueTaskRunner->PostTask(
                    FROM_HERE, ::base::Bind(&C2VdecComponent::DequeueThreadUtil::onAllocBufferTask, ::base::Unretained(this),
                    size, pixelFormat));
}

void C2VdecComponent::DequeueThreadUtil::onAllocBufferTask(media::Size size, uint32_t pixelFormat) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    DCHECK(mDequeueTaskRunner->BelongsToCurrentThread());

    if (!mRunTaskLoop.load()) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR,"onAllocBufferTask failed. thread stopped");
        return;
    }

    int64_t nowTimeUs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000;
    int64_t allocRetryDurationUs = nowTimeUs - mLastAllocBufferRetryTimeUs;
    int64_t allocSuccessDurationUs = nowTimeUs - mLastAllocBufferSuccessTimeUs;

    if ((allocRetryDurationUs <=  mStreamDurationUs) && mAllocBufferLoop.load()) {
        int64_t delayTimeUs = (mStreamDurationUs >= allocRetryDurationUs) ? (mStreamDurationUs - allocRetryDurationUs) : mStreamDurationUs;
        mDequeueTaskRunner->PostDelayedTask(
                    FROM_HERE, ::base::Bind(&C2VdecComponent::DequeueThreadUtil::onAllocBufferTask, ::base::Unretained(this),
                    size, pixelFormat), ::base::TimeDelta::FromMicroseconds(delayTimeUs));
        return;
    }

    if (mFetchBlockCount >= DEFAULT_START_OPTIMIZE_FRAME_NUMBER_MIN) {
        if (allocSuccessDurationUs <= mMinFetchBlockInterval && mAllocBufferLoop.load()) {
            int64_t delayTimeUs = mMinFetchBlockInterval - allocSuccessDurationUs;
            mDequeueTaskRunner->PostDelayedTask(
                FROM_HERE, ::base::Bind(&C2VdecComponent::DequeueThreadUtil::onAllocBufferTask, ::base::Unretained(this),
                size, pixelFormat), ::base::TimeDelta::FromMicroseconds(delayTimeUs));
            return;
        }
    }

    if ((size.width() == 0 || size.height() == 0) || pixelFormat == 0) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR, "dequeueBlockTask size pixel format error and exit.");
        return;
    }

    if (mCurrentBlockSize.width() != size.width() || mCurrentBlockSize.height() != size.height() ||
        mCurrentPixelFormat != pixelFormat) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR, "dequeueBlockTask  size pixel format error and exit.");
        return;
    }
    media::Size videoSize = comp->GetCurrentVideoSize();
    bool resolutionchanging = comp->isResolutionChanging();
    std::shared_ptr<DeviceUtil> deviceUtil = comp->GetDeviceUtil();
    std::shared_ptr<C2VdecBlockPoolUtil> blockPoolUtil = comp->GetBlockPoolUtil();

    if (blockPoolUtil == NULL || deviceUtil == NULL) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR,"device or pool util is null,cancel fetch task.");
        return;
    }
    uint64_t platformUsage = deviceUtil->getPlatformUsage();
    C2MemoryUsage usage = {
            comp->isSecureMode() ? (C2MemoryUsage::READ_PROTECTED | C2MemoryUsage::WRITE_PROTECTED) :
            (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE),  platformUsage};

    uint32_t blockId = 0;
    c2_status_t err = C2_TIMED_OUT;
    C2BlockPool::local_id_t poolId;
    std::shared_ptr<C2GraphicBlock> block;

    if (comp->IsCheckStopDequeueTask()) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR,"the component current state can't deque block. cancel dequeue task.");
        return;
    }
    if (mAllocBufferLoop.load() == false) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR,"the flag of fetch block is false. cancel dequeue task.");
        return;
    }
    if (!resolutionchanging) {
        blockPoolUtil->getPoolId(&poolId);
        auto format = deviceUtil->getStreamPixelFormat(pixelFormat);

        err = blockPoolUtil->fetchGraphicBlock(deviceUtil->getOutAlignedSize(size.width()),
                                        deviceUtil->getOutAlignedSize(size.height()),
                                        format, usage, &block);
    }
    if (err == C2_OK) {
        mLastAllocBufferSuccessTimeUs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000;
        if (videoSize.width() <= block->width() &&
                        videoSize.height() <= block->height()) {
            err = blockPoolUtil->getBlockIdByGraphicBlock(block, &blockId);
            if (err != C2_OK) {
                C2VdecDQ_LOG(CODEC2_LOG_ERR, "get the block id failed, please check it. err:%d", err);
            }

            if (comp->IsCompHaveCurrentBlock(poolId, blockId)) { //old block
                comp->GetTaskRunner()->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onOutputBufferReturned,
                                            ::base::Unretained(comp.get()), std::move(block), poolId, blockId));
            } else { //new block
                comp->GetTaskRunner()->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onNewBlockBufferFetched,
                                ::base::Unretained(comp.get()), std::move(block), poolId, blockId));
            }
        } else {
            C2VdecDQ_LOG(CODEC2_LOG_TAG_BUFFER, "The allocated block size(%d*%d) does not match the current resolution(%d*%d), so discarded it.", block->width(), block->height(),
                    videoSize.width(), videoSize.height());

            if (!blockPoolUtil->isBufferQueue()) {
                blockPoolUtil->resetGraphicBlock(block);
                block.reset();
            }
        }
    } else {
        int32_t delayTime = getFetchGraphicBlockDelayTimeUs(err);
        mLastAllocBufferRetryTimeUs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000;
        C2VdecDQ_LOG(CODEC2_LOG_TAG_BUFFER, "retry dequeue task delay times:%d", delayTime);

        mDequeueTaskRunner->PostDelayedTask(
                    FROM_HERE, ::base::Bind(&C2VdecComponent::DequeueThreadUtil::onAllocBufferTask, ::base::Unretained(this),
                    size, pixelFormat), ::base::TimeDelta::FromMicroseconds(delayTime));
    }

    mFetchBlockCount++;
}

int32_t C2VdecComponent::DequeueThreadUtil::getFetchGraphicBlockDelayTimeUs(c2_status_t err) {
    // Variables used to exponential backOff retry when buffer fetching times out.
    constexpr int kFetchRetryDelayInit = 64;    // Initial delay: 64us
    int kFetchRetryDelayMax = DEFAULT_FRAME_DURATION;
    float frameRate = 0.0f;
    int perFrameDur = 0;
    static int sDelay = kFetchRetryDelayInit;
    LockWeakPtrWithReturnVal(comp, mComp , 0);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl , 0);

    mDeviceUtil = comp->GetDeviceUtil();
    LockWeakPtrWithReturnVal(deviceUtil, mDeviceUtil, 0);

    frameRate = intfImpl->getInputFrameRate();
    if (frameRate > 0.0f) {
        perFrameDur = (int) (1000 / frameRate) * 1000;
        if (deviceUtil != nullptr && deviceUtil->isInterlaced())
            perFrameDur = perFrameDur / 2;
        perFrameDur = std::max(perFrameDur, 2 * kFetchRetryDelayInit);
        kFetchRetryDelayMax = std::min(perFrameDur, kFetchRetryDelayMax);
    }
    if (err == C2_TIMED_OUT || err == C2_BLOCKING || err == C2_NO_MEMORY) {
        C2VdecDQ_LOG(CODEC2_LOG_TAG_BUFFER, "[%s] fetchGraphicBlock() timeout, waiting %d us frameRate:%f perFrameDur:%d kFetchRetryDelayMax:%d", __func__,
                     sDelay, frameRate, perFrameDur, kFetchRetryDelayMax);
        sDelay = std::min(kFetchRetryDelayMax, kFetchRetryDelayMax);  // Exponential backOff
    } else {
        sDelay = perFrameDur;
    }

    return sDelay;
}

}
