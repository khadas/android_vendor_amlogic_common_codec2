/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
#ifndef _C2VDEC_TUNNERPASSTHROUGH_H_
#define _C2VDEC_TUNNERPASSTHROUGH_H_

#include <mutex>

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>

#include <C2VdecComponent.h>

#include <C2VdecBlockPoolUtil.h>
#include <VideoTunnelRendererWraper.h>
#include <C2VdecTunnelHelper.h>

namespace android {

class C2VdecComponent::TunerPassthroughHelper {
public:
    TunerPassthroughHelper(C2VdecComponent* comp, bool secure, const char* mime, C2VdecComponent::TunnelHelper *tunnelHelper);
    TunerPassthroughHelper(C2VdecComponent* comp, bool secure);
    virtual ~TunerPassthroughHelper();

    c2_status_t start();
    c2_status_t stop();
    c2_status_t flush();
    c2_status_t setTrickMode();

private:
    static int notifyTunerPassthroughRenderTimeCallback(void* obj, void* args);
    int postNotifyRenderTimeTunerPassthrough(struct renderTime* rendertime);
    void onNotifyRenderTimeTunerPassthrough(struct renderTime rendertime);
    int sendOutputBufferToWorkTunerPassthrough(struct renderTime* rendertime);
    C2VdecComponent* mComp;
    C2VdecComponent::TunnelHelper *mTunnelHelper;
    C2VdecComponent::IntfImpl* mIntfImpl;
    scoped_refptr<::base::SingleThreadTaskRunner> mTaskRunner;
    int32_t mSyncId;
    passthroughInitParams mTunerPassthroughParams;
    TunerPassthroughWrapper *mTunerPassthrough;
};

}
#endif
