/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "TunerPassthroughWrapper"
#include <dlfcn.h>
#include <unistd.h>
#include <stdlib.h>
#include <utils/Log.h>
#include <C2VendorProperty.h>
#include <c2logdebug.h>
#include "TunerPassthroughWrapper.h"
#include "VideoTunnelRendererBase.h"

namespace android {

static void *gMediaHal = NULL;

uint32_t TunerPassthroughWrapper::gInstanceNum = 0;
uint32_t TunerPassthroughWrapper::gInstanceCnt = 0;

#define C2VdecTPWraper_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, mInstanceCnt, TunerPassthroughWrapper::gInstanceNum, ##str)


static TunerPassthroughBase* getTunerPassthrough() {
    //default version is 1.0
    //uint32_t versionM = 1;
    //uint32_t versionL = 0;
    if (gMediaHal == NULL) {
        gMediaHal = dlopen("libmediahal_passthrough.so", RTLD_NOW);
        if (gMediaHal == NULL) {
            CODEC2_LOG(CODEC2_LOG_ERR,"Unable to dlopen libmediahal_passthrough: %s", dlerror());
            return NULL;
        }
    }

    typedef TunerPassthroughBase *(*createTunerPassthroughFunc)();

    createTunerPassthroughFunc getTunerPassthrough = NULL;

    getTunerPassthrough =
        (createTunerPassthroughFunc)dlsym(gMediaHal, "TunerPassthroughBase_create");

    if (getTunerPassthrough == NULL) {
        dlclose(gMediaHal);
        gMediaHal = NULL;
        CODEC2_LOG(CODEC2_LOG_ERR,"not get TunerPassthroughBase_create\n");
        return NULL;
    }

    TunerPassthroughBase* halHandle = (*getTunerPassthrough)();
    CODEC2_LOG(CODEC2_LOG_INFO,"[%s/%d] get TunerPassthroughBase_create ok\n", __FUNCTION__, __LINE__);
    return halHandle;
}

TunerPassthroughWrapper::TunerPassthroughWrapper() {
    mTunerPassthrough = getTunerPassthrough();
    gInstanceCnt++;
    gInstanceNum++;
    mInstanceCnt = gInstanceCnt;
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    C2VdecTPWraper_LOG(CODEC2_LOG_INFO, "Create");
}

TunerPassthroughWrapper::~TunerPassthroughWrapper() {
    C2VdecTPWraper_LOG(CODEC2_LOG_INFO,"Destroy");
    if (mTunerPassthrough) {
        delete mTunerPassthrough;
        mTunerPassthrough = NULL;
    }
    gInstanceNum--;
}

int TunerPassthroughWrapper::initialize(passthroughInitParams* params) {
    C2VdecTPWraper_LOG(CODEC2_LOG_INFO,"initialize");
    if (!mTunerPassthrough) {
        C2VdecTPWraper_LOG(CODEC2_LOG_ERR,"mTunerPassthrough is NULL!\n");
        return -1;
    }
    if (mTunerPassthrough->Init(params) != 0) {
        C2VdecTPWraper_LOG(CODEC2_LOG_ERR,"init tuner passthrough error!\n");
        return -1;
    }
    return 0;
}

int TunerPassthroughWrapper::regNotifyTunnelRenderTimeCallBack(callbackFunc funs, void* obj) {
    C2VdecTPWraper_LOG(CODEC2_LOG_INFO,"regNotifyTunnelRenderTimeCallBack");
    if (!mTunerPassthrough)
        return false;
    return mTunerPassthrough->RegCallBack(VideoTunnelRendererBase::CB_NODIFYRENDERTIME, funs, obj);
}

int TunerPassthroughWrapper::start() {
    C2VdecTPWraper_LOG(CODEC2_LOG_INFO,"start");
    if (mTunerPassthrough)
        mTunerPassthrough->Start();
    return 0;
}

int TunerPassthroughWrapper::stop() {
    C2VdecTPWraper_LOG(CODEC2_LOG_INFO,"stop");
    if (mTunerPassthrough)
        mTunerPassthrough->Stop();
    return 0;

}
int TunerPassthroughWrapper::flush() {
    C2VdecTPWraper_LOG(CODEC2_LOG_INFO,"flush");
    if (mTunerPassthrough)
        mTunerPassthrough->Flush();
    return 0;
}

int TunerPassthroughWrapper::getSyncInstansNo(int *no) {
    C2VdecTPWraper_LOG(CODEC2_LOG_INFO,"getSyncInstansNo");
    if (mTunerPassthrough)
        mTunerPassthrough->GetSyncInstansNo(no);
    return 0;
}
int TunerPassthroughWrapper::SetTrickMode(int mode) {
    C2VdecTPWraper_LOG(CODEC2_LOG_INFO,"SetTrickMode");
    if (mTunerPassthrough)
        mTunerPassthrough->SetTrickMode(mode);
    return 0;
}
int TunerPassthroughWrapper::SetTrickSpeed(float speed) {
    C2VdecTPWraper_LOG(CODEC2_LOG_INFO,"SetTrickSpeed");
    if (mTunerPassthrough)
        mTunerPassthrough->SetTrickSpeed(speed);
    return 0;
}



}



