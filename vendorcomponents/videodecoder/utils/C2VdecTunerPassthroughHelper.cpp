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

#include <c2logdebug.h>
#include <C2VdecTunerPassthroughHelper.h>
#include <c2logdebug.h>
#include <C2VdecInterfaceImpl.h>

#define HWSYNCID_PASSTHROUGH_FLAG (1u << 16)

#define C2VdecTPH_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, C2VdecComponent::mInstanceID, mComp->mCurInstanceID, ##str)
#define RETURN_ON_UNINITIALIZED_OR_ERROR()                                 \
    do {                                                                   \
        if (mComp->mHasError \
            || mComp->mComponentState == ComponentState::UNINITIALIZED \
            || mComp->mComponentState == ComponentState::DESTROYING \
            || mComp->mComponentState == ComponentState::DESTROYED) \
            return;                                                        \
    } while (0)

namespace android {

C2VdecComponent::TunerPassthroughHelper::TunerPassthroughHelper(C2VdecComponent* comp, bool secure, const char* mime, C2VdecComponent::TunnelHelper *tunnelHelper):
    mComp(comp),
    mTunnelHelper(tunnelHelper) {
    DCHECK(mComp!=NULL);
    mIntfImpl = mComp->GetIntfImpl();
    mTaskRunner = mComp->GetTaskRunner();
    DCHECK(mTaskRunner != NULL);

    mSyncId = mIntfImpl->mVendorTunerHalParam->hwAVSyncId;
    if ((mSyncId & 0x0000FF00) == 0xFF00 || mSyncId == 0x0) {
        mSyncId = mSyncId | HWSYNCID_PASSTHROUGH_FLAG;
    } else {
        C2VdecTPH_LOG(CODEC2_LOG_ERR, "Invalid hwsyncid:0x%x", mSyncId);
    }

    int32_t filterid = mIntfImpl->mVendorTunerHalParam->videoFilterId;
    mTunerPassthroughParams.dmx_id = (filterid >> 16);
    mTunerPassthroughParams.video_pid = (filterid & 0x0000FFFF);
    mTunerPassthroughParams.secure_mode = secure;
    mTunerPassthroughParams.mime = mime;
    mTunerPassthroughParams.tunnel_renderer = mTunnelHelper->getTunnelRender();
    mTunerPassthroughParams.hw_sync_id = mSyncId;
    mTunerPassthrough = new TunerPassthroughWrapper();
    mTunerPassthrough->initialize(&mTunerPassthroughParams);
    mTunerPassthrough->regNotifyTunnelRenderTimeCallBack(notifyTunerPassthroughRenderTimeCallback, this);

    C2VdecTPH_LOG(CODEC2_LOG_INFO, "[%s] passthrough dmxid:%d,vpid:%d,syncid:%0xx",
        __func__,
        mTunerPassthroughParams.dmx_id,
        mTunerPassthroughParams.video_pid,
        mTunerPassthroughParams.hw_sync_id);
}

C2VdecComponent::TunerPassthroughHelper::~TunerPassthroughHelper() {
    delete mTunerPassthrough;
    mTunerPassthrough = NULL;
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
    return C2_OK;
}

void C2VdecComponent::TunerPassthroughHelper::onNotifyRenderTimeTunerPassthrough(struct VideoTunnelRendererWraper::renderTime rendertime) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    struct VideoTunnelRendererWraper::renderTime renderTime = {
        .mediaUs = rendertime.mediaUs,
        .renderUs = rendertime.renderUs,
    };
    C2VdecTPH_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s:%d] Rendertime:%lld", __func__, __LINE__, renderTime.mediaUs);
    sendOutputBufferToWorkTunerPassthrough(&renderTime);
}

int C2VdecComponent::TunerPassthroughHelper::notifyTunerPassthroughRenderTimeCallback(void* obj, void* args) {
    C2VdecComponent::TunerPassthroughHelper* pPassthroughHelper = (C2VdecComponent::TunerPassthroughHelper*)obj;
    struct VideoTunnelRendererWraper::renderTime* rendertime = (struct VideoTunnelRendererWraper::renderTime*)args;
    pPassthroughHelper->postNotifyRenderTimeTunerPassthrough(rendertime);
    return 0;
}
int C2VdecComponent::TunerPassthroughHelper::postNotifyRenderTimeTunerPassthrough(struct VideoTunnelRendererWraper::renderTime* rendertime) {
    struct VideoTunnelRendererWraper::renderTime renderTime = {
        .mediaUs = rendertime->mediaUs,
        .renderUs = rendertime->renderUs,
    };
    mTaskRunner->PostTask(FROM_HERE,
        ::base::Bind(&C2VdecComponent::TunerPassthroughHelper::onNotifyRenderTimeTunerPassthrough, ::base::Unretained(this),
            base::Passed(&renderTime)));
    return 0;
}

int C2VdecComponent::TunerPassthroughHelper::sendOutputBufferToWorkTunerPassthrough(struct VideoTunnelRendererWraper::renderTime* rendertime) {
    std::unique_ptr<C2Work> work(new C2Work);

    work->worklets.clear();
    work->worklets.emplace_back(new C2Worklet);
    work->worklets.front()->output.ordinal.timestamp = rendertime->mediaUs;
    mIntfImpl->mTunnelSystemTimeOut->value = rendertime->renderUs * 1000;
    work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*(mIntfImpl->mTunnelSystemTimeOut)));
    mComp->reportWork(std::move(work));

    return true;
}

}
