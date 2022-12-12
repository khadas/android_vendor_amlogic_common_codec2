/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
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
    DequeueThreadUtil(C2VdecComponent* comp);
    virtual ~DequeueThreadUtil();
    bool StartRunDequeueTask(media::Size size, uint32_t pixelFormat);
    void StopRunDequeueTask();

    void StartAllocBuffer();
    void StopAllocBuffer();
    bool getAllocBufferLoopState();

    void postDelayedAllocTask(media::Size size, uint32_t pixelFormat, bool waitRunning, uint32_t delayTimeUs);
private:
    void onAllocBufferTask(media::Size size, uint32_t pixelFormat);
    int32_t getFetchGraphicBlockDelayTimeUs(c2_status_t err);

    C2VdecComponent* mComp;
    C2VdecComponent::IntfImpl *mIntfImpl;

    ::base::Thread mDequeueThread;
    std::atomic<bool> mRunTaskLoop;
    std::atomic<bool> mAllocBufferLoop;
    scoped_refptr<::base::SingleThreadTaskRunner> mDequeueTaskRunner;

    uint32_t mStreamDurationUs;
    uint32_t mCurrentPixelFormat;
    int64_t mLastAllocBufferRetryTimeUs;
    media::Size mCurrentBlockSize;
};

}

#endif // _C2_Vdec_DEQUEUE_THREAD_H_