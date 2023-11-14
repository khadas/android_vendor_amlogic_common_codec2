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

#ifndef _C2VDEC_TUNNERPASSTHROUGH_H_
#define _C2VDEC_TUNNERPASSTHROUGH_H_

#include <mutex>
#include <memory.h>
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

class C2VdecComponent::TunerPassthroughHelper : public IC2Observer {
public:
    TunerPassthroughHelper(bool secure,
            const char* mime,
            std::shared_ptr<C2VdecComponent::TunnelHelper> tunnelHelper);
    virtual ~TunerPassthroughHelper();

    c2_status_t setComponent(std::shared_ptr<C2VdecComponent> sharedcomp);
    c2_status_t start();
    c2_status_t stop();
    c2_status_t flush();
    c2_status_t setTrickMode();
    c2_status_t setWorkMode();
    c2_status_t setInstanceNo();
    c2_status_t setRenderCallBackEventFlag();

private:
    static int notifyTunerPassthroughRenderTimeCallback(void* obj, void* args);
    int postNotifyRenderTimeTunerPassthrough(struct renderTime* rendertime);
    void onNotifyRenderTimeTunerPassthrough(struct renderTime rendertime);
    int sendOutputBufferToWorkTunerPassthrough(struct renderTime* rendertime);
    std::weak_ptr<C2VdecComponent> mComp;
    std::weak_ptr<C2VdecComponent::TunnelHelper> mTunnelHelper;
    std::weak_ptr<C2VdecComponent::IntfImpl> mIntfImpl;
    int32_t mSyncId;
    int32_t mWorkMode;
    int32_t mPlayerSyncNum;
    passthroughInitParams mTunerPassthroughParams;
    std::shared_ptr<TunerPassthroughWrapper> mTunerPassthrough;
};

}
#endif
