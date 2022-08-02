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
#include <utils/Log.h>
#include "TunerPassthroughWrapper.h"
#include "VideoTunnelRendererBase.h"

namespace android {

static void *gMediaHal = NULL;

static TunerPassthroughBase* getTunerPassthrough() {
    //default version is 1.0
    //uint32_t versionM = 1;
    //uint32_t versionL = 0;
    if (gMediaHal == NULL) {
        gMediaHal = dlopen("libmediahal_passthrough.so", RTLD_NOW);
        if (gMediaHal == NULL) {
            ALOGE("[%s/%d] unable to dlopen libmediahal_passthrough: %s",
                __FUNCTION__, __LINE__, dlerror());
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
        ALOGE("[%s/%d] can not get TunerPassthroughBase_create\n", __FUNCTION__, __LINE__);
        return NULL;
    }

    TunerPassthroughBase* halHandle = (*getTunerPassthrough)();
    ALOGD("[%s/%d] get TunerPassthroughBase_create ok\n", __FUNCTION__, __LINE__);
    return halHandle;
}

TunerPassthroughWrapper::TunerPassthroughWrapper() {
    mTunerPassthrough = getTunerPassthrough();
    ALOGD("[%s/%d]", __FUNCTION__, __LINE__);
}

TunerPassthroughWrapper::~TunerPassthroughWrapper() {
    ALOGD("[%s/%d]", __FUNCTION__, __LINE__);
    if (mTunerPassthrough) {
        delete mTunerPassthrough;
        mTunerPassthrough = NULL;
    }
}

int TunerPassthroughWrapper::initialize(passthroughInitParams* params) {

    if (!mTunerPassthrough) {
        ALOGE("[%s/%d] mTunerPassthrough is NULL!\n", __FUNCTION__, __LINE__);
        return -1;
    }

    if (mTunerPassthrough->Init(params) != 0) {
        ALOGE("[%s/%d] init tuner passthrough error!\n", __FUNCTION__, __LINE__);
        return -1;
    }

    return 0;
}

int TunerPassthroughWrapper::regNotifyTunnelRenderTimeCallBack(callbackFunc funs, void* obj) {
    if (!mTunerPassthrough)
        return false;
    return mTunerPassthrough->RegCallBack(VideoTunnelRendererBase::CB_NODIFYRENDERTIME, funs, obj);
}

int TunerPassthroughWrapper::start() {
    ALOGD("[%s/%d]", __FUNCTION__, __LINE__);

    if (mTunerPassthrough) {
        mTunerPassthrough->Start();
    }
    return 0;
}
int TunerPassthroughWrapper::stop() {
    ALOGD("[%s/%d]", __FUNCTION__, __LINE__);

    if (mTunerPassthrough) {
        mTunerPassthrough->Stop();
    }
    return 0;

}
int TunerPassthroughWrapper::getSyncInstansNo(int *no) {
    ALOGD("[%s/%d]", __FUNCTION__, __LINE__);

    if (mTunerPassthrough) {
        mTunerPassthrough->GetSyncInstansNo(no);
    }
    return 0;
}

}



