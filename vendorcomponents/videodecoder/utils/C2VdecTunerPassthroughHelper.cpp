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

#define LOG_TAG "TunerPassthroughHelper"

#include <base/bind.h>
#include <base/bind_helpers.h>
#include <C2AllocatorGralloc.h>

#include <am_gralloc_ext.h>

#include <C2VendorProperty.h>
#include <C2VdecTunerPassthroughHelper.h>
#include <C2VendorDebug.h>
#include <C2VdecInterfaceImpl.h>

#define HWSYNCID_PASSTHROUGH_FLAG (1u << 16)

#define C2VdecTPH_LOG(level, fmt, str...) CODEC2_LOG(level, "[NO-%d]-[%d]"#fmt, mPlayerSyncNum, C2VdecComponent::mInstanceNum, ##str)

enum TRICK_MODE {
    TRICKMODE_SMOOTH = 1, //based on the playback rate of the codec
    TRICKMODE_BY_SEEK, //playback speed is achieved by changing the play position
    TRICKMODE_MAX,
};

enum {
    TRICK_MODE_NONE = 0,          // Disable trick mode
    TRICK_MODE_PAUSE = 1,         // Pause the video decoder
    TRICK_MODE_PAUSE_NEXT = 2,    // Pause the video decoder when a new frame displayed
    TRICK_MODE_IONLY = 3          // Decoding and Out I frame only
};

enum WORK_MODE {
    WORKMODE_NORMAL = 0, //playback in normal mode
    WORKMODE_CACHE, //playback in cache mode (used in fcc function)
    WORKMODE_MAX,
};

enum {
    WORK_MODE_NORMAL = 0,             // Normal mode
    WORK_MODE_CACHING_ONLY = 1,       // Only caching data, do not decode. Used in FCC
    WORK_MODE_DECODE_ONLY = 2         // Decode data but do not output
};


namespace android {

C2VdecComponent::TunerPassthroughHelper::TunerPassthroughHelper(bool secure,
        const char* mime,
        std::shared_ptr<C2VdecComponent::TunnelHelper> tunnelHelper) {
    mTunnelHelper = tunnelHelper;

    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    mTunerPassthroughParams.secure_mode = secure;
    mTunerPassthroughParams.mime = mime;
    mTunerPassthroughParams.tunnel_renderer = tunnelHelper->getTunnelRender();
    mTunerPassthroughParams.dmx_id = -1;
    mTunerPassthroughParams.video_pid = -1;
    mTunerPassthroughParams.hw_sync_id = -1;
    CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);

    mPlayerSyncNum = -1;
    mSyncId = 0;
    mWorkMode = WORKMODE_NORMAL;
}

C2VdecComponent::TunerPassthroughHelper::~TunerPassthroughHelper() {
    if (mTunerPassthrough) {
        mTunerPassthrough->regNotifyTunnelRenderTimeCallBack(NULL, NULL);
        mTunerPassthrough.reset();
        mTunerPassthrough = NULL;
    }
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::setComponent(std::shared_ptr<C2VdecComponent> sharedcomp) {
    mComp = sharedcomp;
    std::shared_ptr<C2VdecComponent::IntfImpl> intfImpl = sharedcomp->GetIntfImpl();
    mIntfImpl = intfImpl;

    mSyncId = intfImpl->mVendorTunerHalParam->hwAVSyncId;
    if ((mSyncId & 0x0000FF00) == 0xFF00 || mSyncId == 0x0) {
        mSyncId = mSyncId | HWSYNCID_PASSTHROUGH_FLAG;
    } else {
        C2VdecTPH_LOG(CODEC2_LOG_ERR, "Invalid hwsyncid:0x%x", mSyncId);
    }

    LockWeakPtrWithReturnVal_WithoutC2Status(helper, mTunnelHelper, C2_BAD_VALUE);
    if (helper->getTunnelRender()) {
        helper->getTunnelRender()->init(mSyncId);
    }

    int32_t filterid = intfImpl->mVendorTunerHalParam->videoFilterId;
    mTunerPassthroughParams.dmx_id = ((filterid >> 16) & 0x0000000F);
    mTunerPassthroughParams.video_pid = (filterid & 0x0000FFFF);
    mTunerPassthroughParams.hw_sync_id = mSyncId;
    mWorkMode = intfImpl->mVendorTunerPassthroughWorkMode->workMode;
    int32_t playerInstanceNo = intfImpl->mVendorTunerPassthroughInstanceNo->instanceNo;

    mPlayerSyncNum = mSyncId & 0x00FF;

    if (!mTunerPassthrough) {
        mTunerPassthrough = std::make_shared<TunerPassthroughWrapper>();
        if (playerInstanceNo >= 0) {
            mTunerPassthrough->setInstanceNo(playerInstanceNo);
        }
        mTunerPassthrough->initialize(&mTunerPassthroughParams);
        mTunerPassthrough->regNotifyTunnelRenderTimeCallBack(notifyTunerPassthroughRenderTimeCallback, this);
        mTunerPassthrough->SetWorkMode(mWorkMode);

        C2VdecTPH_LOG(CODEC2_LOG_INFO, "[%s] passthrough work mode mVideoFilterId:%x,dmxid:%d,vpid:%d,syncid:%0xx",
            __func__,
            filterid,
            mTunerPassthroughParams.dmx_id,
            mTunerPassthroughParams.video_pid,
            mTunerPassthroughParams.hw_sync_id);
    } else {
        mTunerPassthrough->SetWorkMode(mWorkMode);
        C2VdecTPH_LOG(CODEC2_LOG_INFO, "[%s] passthrough Cache mode not init again",
            __func__);
    }


    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::start() {
    mTunerPassthrough->start();

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::stop() {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);

    mTunerPassthrough->stop();
    //as passthrough stop is synchronous invoke, here need invoke done
    comp->NotifyFlushOrStopDone();

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::flush() {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);

    mTunerPassthrough->flush();
    //as passthrough stop is synchronous invoke, here need invoke done
    comp->NotifyFlushOrStopDone();

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::setTrickMode() {
    LockWeakPtrWithReturnVal_WithoutC2Status(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal_WithoutC2Status(intfImpl, mIntfImpl, C2_BAD_VALUE);

    // int frameAdvance = intfImpl->mVendorTunerPassthroughTrickMode->frameAdvance;
    int mode = intfImpl->mVendorTunerPassthroughTrickMode->trickMode;
    int trickSpeed = intfImpl->mVendorTunerPassthroughTrickMode->trickSpeed;

    // if (frameAdvance)
    //     return C2_OK;

    if (mode  == TRICKMODE_SMOOTH) {
        mode = TRICK_MODE_NONE;
    } else if (mode == TRICKMODE_BY_SEEK) {
        mode = TRICK_MODE_PAUSE_NEXT;
    } else {
        mode = TRICK_MODE_NONE;
    }

    if (trickSpeed == 0)
        trickSpeed = 1000;

    C2VdecTPH_LOG(CODEC2_LOG_INFO, "passthrough trickmode:%d, trickspeed:%d", mode, trickSpeed);

    mTunerPassthrough->SetTrickMode(mode);
    mTunerPassthrough->SetTrickSpeed(trickSpeed / 1000.f);

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::setWorkMode() {
    LockWeakPtrWithReturnVal_WithoutC2Status(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal_WithoutC2Status(intfImpl, mIntfImpl, C2_BAD_VALUE);

    int mode = intfImpl->mVendorTunerPassthroughWorkMode->workMode;

    if (mode  == WORKMODE_NORMAL) {
        mWorkMode = WORK_MODE_NORMAL;
    } else if (mode == WORKMODE_CACHE) {
        mWorkMode = WORK_MODE_CACHING_ONLY;
    } else {
        mWorkMode = WORK_MODE_NORMAL;
    }

    C2VdecTPH_LOG(CODEC2_LOG_INFO, "passthrough workmode:%d", mWorkMode);

    // mTunerPassthrough->SetWorkMode(mode);

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::setInstanceNo() {
    LockWeakPtrWithReturnVal_WithoutC2Status(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal_WithoutC2Status(intfImpl, mIntfImpl, C2_BAD_VALUE);

    int32_t instanceNo = intfImpl->mVendorTunerPassthroughInstanceNo->instanceNo;

    C2VdecTPH_LOG(CODEC2_LOG_INFO, "passthrough instanceNo:%d", instanceNo);

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::setRenderCallBackEventFlag() {
    LockWeakPtrWithReturnVal_WithoutC2Status(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal_WithoutC2Status(intfImpl, mIntfImpl, C2_BAD_VALUE);

    int64_t mask = intfImpl->mVendorTunerPassthroughEventMask->eventMask;

    mTunerPassthrough->SetRenderCallBackEventFlag(mask);

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::setMute() {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, C2_BAD_VALUE);

    int64_t mMute = intfImpl->mVendorTunerPassthroughMute->mMute;

    passthroughParams configParam;
    memset(&configParam, 0, sizeof(passthroughParams));

    configParam.param1 = mMute;
    mTunerPassthrough->SetPassthroughParams(AM_PASSTHROUGH_PARAM_MUTE, &configParam);

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::setScreenColor() {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, C2_BAD_VALUE);

    int32_t screenColor = intfImpl->mVendorTunerPassthroughScreenColor->screenColor;
    int32_t screenMode = intfImpl->mVendorTunerPassthroughScreenColor->screenMode;

    passthroughParams configParam;
    memset(&configParam, 0, sizeof(passthroughParams));

    configParam.param1 = screenColor;
    configParam.param2 = screenMode;
    mTunerPassthrough->SetPassthroughParams(AM_PASSTHROUGH_PARAM_SCREEN_COLOR, &configParam);

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::setTransitionModeBefore() {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, C2_BAD_VALUE);

    int64_t transitionModeBefore = intfImpl->mVendorTunerPassthroughTransitionModeBefore->transitionModeBefore;

    passthroughParams configParam;
    memset(&configParam, 0, sizeof(passthroughParams));

    configParam.param1 = transitionModeBefore;
    mTunerPassthrough->SetPassthroughParams(AM_PASSTHROUGH_PARAM_TRANSITION_MODE_BEFORE, &configParam);

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::setTransitionModeAfter() {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, C2_BAD_VALUE);

    int64_t transitionModeAfter = intfImpl->mVendorTunerPassthroughTransitionModeAfter->transitionModeAfter;

    passthroughParams configParam;
    memset(&configParam, 0, sizeof(passthroughParams));

    configParam.param1 = transitionModeAfter;
    mTunerPassthrough->SetPassthroughParams(AM_PASSTHROUGH_PARAM_TRANSITION_MODE_AFTER, &configParam);

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::setTransitionPrerollRate() {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, C2_BAD_VALUE);

    int64_t transitionPrerollRate = intfImpl->mVendorTunerPassthroughTransitionPrerollRate->transitionPrerollRate;

    passthroughParams configParam;
    memset(&configParam, 0, sizeof(passthroughParams));

    configParam.param1 = transitionPrerollRate;
    mTunerPassthrough->SetPassthroughParams(AM_PASSTHROUGH_PARAM_TRANSITION_PREROLL_RATE, &configParam);

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::setTransitionPrerollAVTolerance() {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, C2_BAD_VALUE);

    int64_t transitionPrerollAVTolerance = intfImpl->mVendorTunerPassthroughPrerollAVTolerance->transitionPrerollAVTolerance;

    passthroughParams configParam;
    memset(&configParam, 0, sizeof(passthroughParams));

    configParam.param1 = transitionPrerollAVTolerance;
    mTunerPassthrough->SetPassthroughParams(AM_PASSTHROUGH_PARAM_TRANSITION_PREROLL_AV_TOLERANCE, &configParam);

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::setPlaybackStatus() {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, C2_BAD_VALUE);

    int64_t mPlaybackStatus = intfImpl->mVendorTunerPassthroughPlaybackStatus->playbackStatus;

    passthroughParams configParam;
    memset(&configParam, 0, sizeof(passthroughParams));

    configParam.param1 = mPlaybackStatus;
    mTunerPassthrough->SetPassthroughParams(AM_PASSTHROUGH_PARAM_PLAYBACK_STATUS, &configParam);

    return C2_OK;
}

void C2VdecComponent::TunerPassthroughHelper::onNotifyRenderTimeTunerPassthrough(struct renderTime rendertime) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    scoped_refptr<::base::SingleThreadTaskRunner> taskRunner = comp->GetTaskRunner();
    if (taskRunner == nullptr) {
        return;
    }
    DCHECK(taskRunner->BelongsToCurrentThread());

    struct renderTime renderTime = {
        .mediaUs = rendertime.mediaUs,
        .renderUs = rendertime.renderUs,
    };
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s:%d] Rendertime:%" PRId64"", __func__, __LINE__, renderTime.mediaUs);
    sendOutputBufferToWorkTunerPassthrough(&renderTime);
}

int C2VdecComponent::TunerPassthroughHelper::notifyTunerPassthroughRenderTimeCallback(void* obj, void* args) {
    if (obj == NULL || args == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR,"%s args error, please check it.", __func__);
        return -1;
    }

    C2VdecComponent::TunerPassthroughHelper* pPassthroughHelper = (C2VdecComponent::TunerPassthroughHelper*)obj;
    struct renderTime* rendertime = (struct renderTime*)args;
    pPassthroughHelper->postNotifyRenderTimeTunerPassthrough(rendertime);
    return 0;
}
int C2VdecComponent::TunerPassthroughHelper::postNotifyRenderTimeTunerPassthrough(struct renderTime* rendertime) {
    LockWeakPtrWithReturnVal(comp, mComp, -1);
    scoped_refptr<::base::SingleThreadTaskRunner> taskRunner = comp->GetTaskRunner();
    if (taskRunner == nullptr) {
        return -1;
    }

    if (rendertime == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR,"%s args error, please check it.", __func__);
        return -1;
    }

    struct renderTime renderTime = {
        .mediaUs = rendertime->mediaUs,
        .renderUs = rendertime->renderUs,
    };
    taskRunner->PostTask(FROM_HERE,
        ::base::Bind(&C2VdecComponent::TunerPassthroughHelper::onNotifyRenderTimeTunerPassthrough, ::base::Unretained(this),
            ::base::Passed(&renderTime)));
    return 0;
}

int C2VdecComponent::TunerPassthroughHelper::sendOutputBufferToWorkTunerPassthrough(struct renderTime* rendertime) {
    LockWeakPtrWithReturnVal(comp, mComp, false);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, false);
    std::unique_ptr<C2Work> work(new C2Work);

    if (work != NULL) {
        work->worklets.clear();
        work->worklets.emplace_back(new C2Worklet);
        work->worklets.front()->output.ordinal.timestamp = rendertime->mediaUs;
        intfImpl->mTunnelSystemTimeOut->value = rendertime->renderUs * 1000;
        work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*(intfImpl->mTunnelSystemTimeOut)));
        comp->reportWork(std::move(work));
    }
    return true;
}

}
