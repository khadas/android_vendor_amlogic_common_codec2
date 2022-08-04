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

uint32_t TunerPassthroughWrapper::gInstanceNum = 0;
uint32_t TunerPassthroughWrapper::gInstanceCnt = 0;

#define TUNERHAL_LOGV(format, ...) ALOGV("[%d ## %d]%s:%d >" format, mInstanceCnt, gInstanceNum, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define TUNERHAL_LOGW(format, ...) ALOGW("[%d ## %d]%s:%d >" format, mInstanceCnt, gInstanceNum, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define TUNERHAL_LOGD(format, ...) ALOGD("[%d ## %d]%s:%d >" format, mInstanceCnt, gInstanceNum, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define TUNERHAL_LOGI(format, ...) ALOGI("[%d ## %d]%s:%d >" format, mInstanceCnt, gInstanceNum, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define TUNERHAL_LOGE(format, ...) ALOGE("[%d ## %d]%s:%d >" format, mInstanceCnt, gInstanceNum, __FUNCTION__, __LINE__, ##__VA_ARGS__)


static TunerPassthroughBase* getTunerPassthrough() {
    //default version is 1.0
    //uint32_t versionM = 1;
    //uint32_t versionL = 0;
    if (gMediaHal == NULL) {
        gMediaHal = dlopen("libmediahal_passthrough.so", RTLD_NOW);
        if (gMediaHal == NULL) {
            ALOGE("unable to dlopen libmediahal_passthrough: %s", dlerror());
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
        ALOGE("xcan not get TunerPassthroughBase_create\n");
        return NULL;
    }

    TunerPassthroughBase* halHandle = (*getTunerPassthrough)();
    ALOGD("[%s/%d] get TunerPassthroughBase_create ok\n", __FUNCTION__, __LINE__);
    return halHandle;
}

TunerPassthroughWrapper::TunerPassthroughWrapper() {
    mTunerPassthrough = getTunerPassthrough();
    gInstanceCnt++;
    gInstanceNum++;
    mInstanceCnt = gInstanceCnt;
    TUNERHAL_LOGD("Create");
}

TunerPassthroughWrapper::~TunerPassthroughWrapper() {
    TUNERHAL_LOGD("Destory");
    if (mTunerPassthrough) {
        delete mTunerPassthrough;
        mTunerPassthrough = NULL;
    }
    gInstanceNum--;
}

int TunerPassthroughWrapper::initialize(passthroughInitParams* params) {
    TUNERHAL_LOGD("initialize");
    if (!mTunerPassthrough) {
        TUNERHAL_LOGE("mTunerPassthrough is NULL!\n");
        return -1;
    }
    if (mTunerPassthrough->Init(params) != 0) {
        TUNERHAL_LOGE("init tuner passthrough error!\n");
        return -1;
    }
    return 0;
}

int TunerPassthroughWrapper::regNotifyTunnelRenderTimeCallBack(callbackFunc funs, void* obj) {
    TUNERHAL_LOGD("regNotifyTunnelRenderTimeCallBack");
    if (!mTunerPassthrough)
        return false;
    return mTunerPassthrough->RegCallBack(VideoTunnelRendererBase::CB_NODIFYRENDERTIME, funs, obj);
}

int TunerPassthroughWrapper::start() {
    TUNERHAL_LOGD("start");
    if (mTunerPassthrough)
        mTunerPassthrough->Start();
    return 0;
}

int TunerPassthroughWrapper::stop() {
    TUNERHAL_LOGD("stop");
    if (mTunerPassthrough)
        mTunerPassthrough->Stop();
    return 0;

}

int TunerPassthroughWrapper::getSyncInstansNo(int *no) {
    TUNERHAL_LOGD("getSyncInstansNo");
    if (mTunerPassthrough)
        mTunerPassthrough->GetSyncInstansNo(no);
    return 0;
}

}



