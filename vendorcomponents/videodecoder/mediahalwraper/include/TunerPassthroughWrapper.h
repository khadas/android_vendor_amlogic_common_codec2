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
    int setInstanceNo(int32_t numb);
    int SetTrickMode(int mode);
    int SetTrickSpeed(float speed);
    int SetWorkMode(int mode);
    int SetRenderCallBackEventFlag(int64_t eventflag);
    int SetPassthroughParams(int32_t type, passthroughParams* params);

private:
    static uint32_t gInstanceCnt;
    static uint32_t gInstanceNum;
    int32_t mPassthroughSyncNum;
    TunerPassthroughBase* mTunerPassthrough;
};

}
#endif
