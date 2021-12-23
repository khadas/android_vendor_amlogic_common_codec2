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
    int getSyncInstansNo(int *no);

private:
    TunerPassthroughBase* mTunerPassthrough;
};
}
#endif
