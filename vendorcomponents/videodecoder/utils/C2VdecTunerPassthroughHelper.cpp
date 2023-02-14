/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */

#define LOG_NDEBUG 0

#define LOG_TAG "TunerPassthroughHelper"

#include <base/bind.h>
#include <base/bind_helpers.h>
#include <C2AllocatorGralloc.h>

#include <am_gralloc_ext.h>

#include <C2VendorProperty.h>
#include <c2logdebug.h>
#include <C2VdecTunerPassthroughHelper.h>
#include <c2logdebug.h>
#include <C2VdecInterfaceImpl.h>

#define HWSYNCID_PASSTHROUGH_FLAG (1u << 16)

#define C2VdecTPH_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, mComp->mCurInstanceID, C2VdecComponent::mInstanceNum, ##str)
#define RETURN_ON_UNINITIALIZED_OR_ERROR()                                 \
    do {                                                                   \
        if (mComp->mHasError \
            || mComp->mComponentState == ComponentState::UNINITIALIZED \
            || mComp->mComponentState == ComponentState::DESTROYING \
            || mComp->mComponentState == ComponentState::DESTROYED) \
            return;                                                        \
    } while (0)

enum TRICK_MODE {
    TRICKMODE_SMOOTH = 1, //based on the playback rate of the codec
    TRICKMODE_BY_SEEK, //playback speed is achieved by changing the play position
    TRICKMODE_MAX,
};

enum {
    TRICK_MODE_NONE = 0,          // Disable trick mode
    TRICK_MODE_PAUSE = 1,         // Pause the video decoder
    TRICK_MODE_PAUSE_NEXT = 2,    // Pause the video decoder when a new frame dispalyed
    TRICK_MODE_IONLY = 3          // Decoding and Out I frame only
};

namespace android {

C2VdecComponent::TunerPassthroughHelper::TunerPassthroughHelper(C2VdecComponent* comp, bool secure, const char* mime, C2VdecComponent::TunnelHelper *tunnelHelper):
    mComp(comp),
    mTunnelHelper(tunnelHelper) {
    DCHECK(mComp!=NULL);
    mIntfImpl = mComp->GetIntfImpl();
    mTaskRunner = mComp->GetTaskRunner();
    DCHECK(mTaskRunner != NULL);
    mSyncId = mIntfImpl->mVendorTunerHalParam->hwAVSyncId;
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    if ((mSyncId & 0x0000FF00) == 0xFF00 || mSyncId == 0x0) {
        mSyncId = mSyncId | HWSYNCID_PASSTHROUGH_FLAG;
    } else {
        C2VdecTPH_LOG(CODEC2_LOG_ERR, "Invalid hwsyncid:0x%x", mSyncId);
    }

    int32_t filterid = mIntfImpl->mVendorTunerHalParam->videoFilterId;
    mTunerPassthroughParams.dmx_id = ((filterid >> 16) & 0x0000000F);
    mTunerPassthroughParams.video_pid = (filterid & 0x0000FFFF);
    mTunerPassthroughParams.secure_mode = secure;
    mTunerPassthroughParams.mime = mime;
    mTunerPassthroughParams.tunnel_renderer = mTunnelHelper->getTunnelRender();
    mTunerPassthroughParams.hw_sync_id = mSyncId;
    mTunerPassthrough = new TunerPassthroughWrapper();
    mTunerPassthrough->initialize(&mTunerPassthroughParams);
    mTunerPassthrough->regNotifyTunnelRenderTimeCallBack(notifyTunerPassthroughRenderTimeCallback, this);

    C2VdecTPH_LOG(CODEC2_LOG_INFO, "[%s] passthrough mVideoFilterId:%x,dmxid:%d,vpid:%d,syncid:%0xx",
        __func__,
        filterid,
        mTunerPassthroughParams.dmx_id,
        mTunerPassthroughParams.video_pid,
        mTunerPassthroughParams.hw_sync_id);
}

C2VdecComponent::TunerPassthroughHelper::~TunerPassthroughHelper() {
    if (mTunerPassthrough) {
        mTunerPassthrough->regNotifyTunnelRenderTimeCallBack(NULL, NULL);
        delete mTunerPassthrough;
        mTunerPassthrough = NULL;
    }
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::start() {
    mTunerPassthrough->start();

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::stop() {
    mTunerPassthrough->stop();
    //as passthrouh stop is synchronous invoke, here need invoke done
    mComp->NotifyFlushOrStopDone();

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::flush() {
    mTunerPassthrough->flush();
    //as passthrouh stop is synchronous invoke, here need invoke done
    mComp->NotifyFlushOrStopDone();

    return C2_OK;
}

c2_status_t C2VdecComponent::TunerPassthroughHelper::setTrickMode() {
    // int frameAdvance = mIntfImpl->mVendorTunerPassthroughTrickMode->frameAdvance;
    int mode = mIntfImpl->mVendorTunerPassthroughTrickMode->trickMode;
    int trickSpeed = mIntfImpl->mVendorTunerPassthroughTrickMode->trickSpeed / 1000;

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
        trickSpeed = 1;

    C2VdecTPH_LOG(CODEC2_LOG_INFO, "passthrough trickmode:%d, trickspeed:%d", mode, trickSpeed);

    mTunerPassthrough->SetTrickMode(mode);
    mTunerPassthrough->SetTrickSpeed(trickSpeed);

    return C2_OK;
}

void C2VdecComponent::TunerPassthroughHelper::onNotifyRenderTimeTunerPassthrough(struct renderTime rendertime) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();

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
    if (rendertime == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR,"%s args error, please check it.", __func__);
        return -1;
    }
    struct renderTime renderTime = {
        .mediaUs = rendertime->mediaUs,
        .renderUs = rendertime->renderUs,
    };
    mTaskRunner->PostTask(FROM_HERE,
        ::base::Bind(&C2VdecComponent::TunerPassthroughHelper::onNotifyRenderTimeTunerPassthrough, ::base::Unretained(this),
            ::base::Passed(&renderTime)));
    return 0;
}

int C2VdecComponent::TunerPassthroughHelper::sendOutputBufferToWorkTunerPassthrough(struct renderTime* rendertime) {
    std::unique_ptr<C2Work> work(new C2Work);

    if (work != NULL && mComp != NULL) {
        work->worklets.clear();
        work->worklets.emplace_back(new C2Worklet);
        work->worklets.front()->output.ordinal.timestamp = rendertime->mediaUs;
        mIntfImpl->mTunnelSystemTimeOut->value = rendertime->renderUs * 1000;
        work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*(mIntfImpl->mTunnelSystemTimeOut)));
        mComp->reportWork(std::move(work));
    }
    return true;
}

}
