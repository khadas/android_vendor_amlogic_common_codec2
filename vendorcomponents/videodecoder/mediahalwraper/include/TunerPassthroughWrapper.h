/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */

#ifndef TUNNER_PASSTHROUGH_WRAPPER_H_
#define TUNNER_PASSTHROUGH_WRAPPER_H_

#include <TunerPassthroughBase.h>

namespace android {

class TunerPassthroughWrapper  {

public:

    TunerPassthroughWrapper();
    ~TunerPassthroughWrapper();

    int initialize(passthroughInitParams* params);
    int regNotifyTunnelRenderTimeCallBack(callbackFunc funs, void* obj);
    int start();
    int stop();
    int flush();
    int getSyncInstansNo(int *no);
    int SetTrickMode(int mode);
    int SetTrickSpeed(float speed);
    int SetRenderCallBackEventFlag(int64_t eventflag);

private:
    static uint32_t gInstanceCnt;
    static uint32_t gInstanceNum;
    uint32_t mInstanceCnt;
    TunerPassthroughBase* mTunerPassthrough;
};

}
#endif
