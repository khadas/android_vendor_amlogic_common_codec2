/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
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

#define C2VdecDQ_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, mComp->mCurInstanceID, C2VdecComponent::mInstanceNum, ##str)

#define DEFAULT_FRAME_DURATION (16384)// default dur: 16ms (1 frame at 60fps)

C2VdecComponent::DequeueThreadUtil::DequeueThreadUtil(C2VdecComponent* comp) {
    mComp = comp;
    DCHECK(mComp != NULL);
    mDequeueThread = new ::base::Thread("C2VdecDequeueThread");
    mIntfImpl = mComp->GetIntfImpl();
    DCHECK(mIntfImpl != NULL);
    mLastAllocBufferRetryTimeUs = -1;
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    C2VdecDQ_LOG(CODEC2_LOG_INFO, "Creat DequeueThreadUtil!!");
    mRunTaskLoop.store(false);
    mAllocBufferLoop.store(false);
    mStreamDurationUs = 0;
    mCurrentPixelFormat = 0;
    memset(&mCurrentBlockSize, 0, sizeof(mCurrentBlockSize));
}

C2VdecComponent::DequeueThreadUtil::~DequeueThreadUtil() {
    C2VdecDQ_LOG(CODEC2_LOG_INFO, "~DequeueThreadUtil!!");
    StopRunDequeueTask();
    if (mDequeueThread != NULL) {
        delete mDequeueThread;
        mDequeueThread = NULL;
    }
}

bool C2VdecComponent::DequeueThreadUtil::StartRunDequeueTask(media::Size size, uint32_t pixelFormat) {

    if (mRunTaskLoop.load() == true) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR,"dequeue thread is running, this is error!! return.");
        return false;
    }
    if (!mDequeueThread->Start()) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR, "Failed to start dequeue thread!!");
        return false;
    }
    mDequeueTaskRunner = mDequeueThread->task_runner();
    mRunTaskLoop.store(true);
    DCHECK(mDequeueTaskRunner != NULL);

    std::shared_ptr<DeviceUtil> deviceUtil = mComp->GetDeviceUtil();
    if (deviceUtil == nullptr) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR, "Failed to start dequeue thread!!");
        return false;
    }
    mStreamDurationUs = deviceUtil->getVideoDurationUs();
    mCurrentBlockSize = size;
    mCurrentPixelFormat = pixelFormat;
    C2VdecDQ_LOG(CODEC2_LOG_TAG_BUFFER,"%s run task loop:%d alloc buffer loop:%d", __func__, mRunTaskLoop.load(), mAllocBufferLoop.load());
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

    mDequeueTaskRunner->PostDelayedTask(
                    FROM_HERE, ::base::Bind(&C2VdecComponent::DequeueThreadUtil::onAllocBufferTask, ::base::Unretained(this),
                    size, pixelFormat), ::base::TimeDelta::FromMicroseconds(delayTimeUs));
}

void C2VdecComponent::DequeueThreadUtil::onAllocBufferTask(media::Size size, uint32_t pixelFormat) {
    DCHECK(mComp != NULL);
    DCHECK(mDequeueTaskRunner->BelongsToCurrentThread());

    if (!mRunTaskLoop.load()) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR,"onAllocBufferTask failed. thread stopped");
        return;
    }

    int64_t allocRetryDurationUs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000 - mLastAllocBufferRetryTimeUs;
    if ((allocRetryDurationUs <=  mStreamDurationUs) && mAllocBufferLoop.load()) {
        int64_t delayTimeUs = (mStreamDurationUs >= allocRetryDurationUs) ? (mStreamDurationUs - allocRetryDurationUs) : mStreamDurationUs;
        mDequeueTaskRunner->PostDelayedTask(
                    FROM_HERE, ::base::Bind(&C2VdecComponent::DequeueThreadUtil::onAllocBufferTask, ::base::Unretained(this),
                    size, pixelFormat), ::base::TimeDelta::FromMicroseconds(delayTimeUs));
        return;
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
    media::Size videoSize = mComp->GetCurrentVideoSize();
    bool resolutionchanging = mComp->isResolutionChanging();
    std::shared_ptr<DeviceUtil> deviceUtil = mComp->GetDeviceUtil();
    std::shared_ptr<C2VdecBlockPoolUtil> blockPoolUtil = mComp->GetBlockPoolUtil();

    if (blockPoolUtil == NULL || deviceUtil == NULL) {
        C2VdecDQ_LOG(CODEC2_LOG_ERR,"device or pool util is null,cancel fetch task.");
        return;
    }
    uint64_t platformUsage = deviceUtil->getPlatformUsage();
    C2MemoryUsage usage = {
            mComp->isSecureMode() ? (C2MemoryUsage::READ_PROTECTED | C2MemoryUsage::WRITE_PROTECTED) :
            (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE),  platformUsage};

    uint32_t blockId = 0;
    c2_status_t err = C2_TIMED_OUT;
    C2BlockPool::local_id_t poolId;
    std::shared_ptr<C2GraphicBlock> block;

    if (mComp->IsCheckStopDequeueTask()) {
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
        if (videoSize.width() <= block->width() &&
                        videoSize.height() <= block->height()) {
            err = blockPoolUtil->getBlockIdByGraphicBlock(block, &blockId);
            if (err != C2_OK) {
                C2VdecDQ_LOG(CODEC2_LOG_ERR, "get the block id failed, please check it. err:%d", err);
            }

            if (mComp->IsCompHaveCurrentBlock(poolId, blockId)) { //old block
                mComp->GetTaskRunner()->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onOutputBufferReturned,
                                            ::base::Unretained(mComp), std::move(block), poolId, blockId));
            } else { //new block
                mComp->GetTaskRunner()->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onNewBlockBufferFetched,
                                ::base::Unretained(mComp), std::move(block), poolId, blockId));
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
}

int32_t C2VdecComponent::DequeueThreadUtil::getFetchGraphicBlockDelayTimeUs(c2_status_t err) {
    // Variables used to exponential backOff retry when buffer fetching times out.
    constexpr int kFetchRetryDelayInit = 64;    // Initial delay: 64us
    int kFetchRetryDelayMax = DEFAULT_FRAME_DURATION;
    float frameRate = 0.0f;
    int perFrameDur = 0;
    static int sDelay = kFetchRetryDelayInit;

    std::shared_ptr<DeviceUtil> deviceUtil = mComp->GetDeviceUtil();

    if (mIntfImpl)
        frameRate = mIntfImpl->getInputFrameRate();
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
