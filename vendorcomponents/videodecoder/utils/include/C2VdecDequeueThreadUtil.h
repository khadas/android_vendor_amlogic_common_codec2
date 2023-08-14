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

#ifndef _C2_Vdec_DEQUEUE_THREAD_H_
#define _C2_Vdec_DEQUEUE_THREAD_H_

#include <rect.h>
#include <size.h>

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>
#include <C2VdecComponent.h>
#include <util/C2InterfaceHelper.h>

namespace android {

class C2VdecComponent::DequeueThreadUtil {
public:
    DequeueThreadUtil();
    virtual ~DequeueThreadUtil();
    c2_status_t setComponent(std::shared_ptr<C2VdecComponent> sharecomp);
    bool StartRunDequeueTask(media::Size size, uint32_t pixelFormat);
    void StopRunDequeueTask();

    void StartAllocBuffer();
    void StopAllocBuffer();
    bool getAllocBufferLoopState();

    void postDelayedAllocTask(media::Size size, uint32_t pixelFormat, bool waitRunning, uint32_t delayTimeUs);
private:
    void onAllocBufferTask(media::Size size, uint32_t pixelFormat);
    int32_t getFetchGraphicBlockDelayTimeUs(c2_status_t err);
    void onInitTask();

    std::weak_ptr<C2VdecComponent> mComp;
    std::weak_ptr<C2VdecComponent::IntfImpl> mIntfImpl;
    std::weak_ptr<DeviceUtil> mDeviceUtil;

    ::base::Thread* mDequeueThread;
    std::atomic<bool> mRunTaskLoop;
    std::atomic<bool> mAllocBufferLoop;
    scoped_refptr<::base::SingleThreadTaskRunner> mDequeueTaskRunner;

    uint32_t mFetchBlockCount;
    uint32_t mStreamDurationUs;
    uint32_t mCurrentPixelFormat;
    int32_t mMinFetchBlockInterval;
    int64_t mLastAllocBufferRetryTimeUs;
    int64_t mLastAllocBufferSuccessTimeUs;
    media::Size mCurrentBlockSize;
    std::ostringstream TRACE_NAME_C2VDEC_DEQUEUE_THREAD;
};

}

#endif // _C2_Vdec_DEQUEUE_THREAD_H_
