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

#define LOG_NDEBUG 0
#define LOG_TAG "VideoTunnelRendererWraper"

#include <VideoTunnelRendererWraper.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unistd.h>
#include <utils/Log.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <C2VendorProperty.h>
#include <C2VendorDebug.h>

namespace android {

void* gMediaHalVideoTunnelRenderer = NULL;

static VideoTunnelRendererBase* getVideoTunnelRenderer() {
    if (!gMediaHalVideoTunnelRenderer) {
        gMediaHalVideoTunnelRenderer = dlopen("libmediahal_tunnelrenderer.so", RTLD_NOW);
        if (gMediaHalVideoTunnelRenderer == NULL) {
            CODEC2_LOG(CODEC2_LOG_ERR,"Unable to dlopen libmediahal_videodec: %s", dlerror());
            return NULL;
        }
    }

    typedef VideoTunnelRendererBase *(*createVideoTunnelRendererFunc)();

    createVideoTunnelRendererFunc getRenderer = NULL;
    getRenderer =
            (createVideoTunnelRendererFunc)dlsym(gMediaHalVideoTunnelRenderer, "VideoTunnelRenderer_create");


    if (getRenderer == NULL) {
        dlclose(gMediaHalVideoTunnelRenderer);
        gMediaHalVideoTunnelRenderer = NULL;
        CODEC2_LOG(CODEC2_LOG_ERR,"Can not create VideoTunnelRenderer_create\n");
        return NULL;
    }

    VideoTunnelRendererBase* RendererHandle = (*getRenderer)();
    CODEC2_LOG(CODEC2_LOG_INFO,"getRenderer ok\n");
    return RendererHandle;
}


VideoTunnelRendererWraper::VideoTunnelRendererWraper(bool secure) {
    mVideoTunnelRenderer = getVideoTunnelRenderer();
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    CODEC2_LOG(CODEC2_LOG_INFO,"mVideoTunnelRenderer:%p\n", mVideoTunnelRenderer);
}

VideoTunnelRendererWraper::~VideoTunnelRendererWraper() {
    CODEC2_LOG(CODEC2_LOG_INFO,"~VideoTunnelRendererWraper ok\n");
    if (mVideoTunnelRenderer) {
        delete mVideoTunnelRenderer;
        mVideoTunnelRenderer = NULL;
    }
}

bool VideoTunnelRendererWraper::init(int hwsyncid) {
    CODEC2_LOG(CODEC2_LOG_INFO,"init:%d\n", hwsyncid);
    if (!mVideoTunnelRenderer) {
        ALOGE("VideoTunnelRenderer is null, init error!\n");
        return false;
    }
    return mVideoTunnelRenderer->init(hwsyncid);
}

bool VideoTunnelRendererWraper::start() {
    if (!mVideoTunnelRenderer)
        return false;

    mVideoTunnelRenderer->start();
    return true;
}

bool VideoTunnelRendererWraper::stop() {
    if (!mVideoTunnelRenderer)
        return false;

    mVideoTunnelRenderer->stop();
    return true;
}

int VideoTunnelRendererWraper::getTunnelId() {
    if (!mVideoTunnelRenderer)
        return -1;
    return mVideoTunnelRenderer->getTunnelId();
}

bool VideoTunnelRendererWraper::sendVideoFrame(int metafd, int64_t timestampNs, bool renderAtOnce) {
    if (!mVideoTunnelRenderer)
        return false;
    return mVideoTunnelRenderer->sendVideoFrame(metafd, timestampNs, renderAtOnce);
}

bool VideoTunnelRendererWraper::peekFirstFrame() {
    if (!mVideoTunnelRenderer)
        return false;
    return mVideoTunnelRenderer->peekFirstFrame();
}

bool VideoTunnelRendererWraper::flush() {
    if (!mVideoTunnelRenderer)
        return false;

    mVideoTunnelRenderer->flush();
    return true;
}

int VideoTunnelRendererWraper::regFillVideoFrameCallBack(callbackFunc funs, void* obj) {
    if (!mVideoTunnelRenderer)
        return false;
    return mVideoTunnelRenderer->regCallBack(VideoTunnelRendererBase::CB_FILLVIDEOFRAME2, funs, obj);
}

int VideoTunnelRendererWraper::regNotifyTunnelRenderTimeCallBack(callbackFunc funs, void* obj) {
    if (!mVideoTunnelRenderer)
        return false;
    return mVideoTunnelRenderer->regCallBack(VideoTunnelRendererBase::CB_NODIFYRENDERTIME, funs, obj);
}

int VideoTunnelRendererWraper::regNotifyEventCallBack(callbackFunc funs, void* obj) {
    if (!mVideoTunnelRenderer)
        return false;
    return mVideoTunnelRenderer->regCallBack(VideoTunnelRendererBase::CB_EVENT, funs, obj);
}

bool VideoTunnelRendererWraper::setFrameRate(int32_t framerate) {
    if (!mVideoTunnelRenderer)
        return false;
#if ANDROID_PLATFORM_SDK_VERSION >= 30
    mVideoTunnelRenderer->setFrameRate(framerate);
#endif
    return true;
}

void VideoTunnelRendererWraper::videoSyncQueueVideoFrame(int64_t timestampUs, uint32_t size) {
    if (!mVideoTunnelRenderer)
        return ;
    return mVideoTunnelRenderer->onVideoSyncQueueVideoFrame(timestampUs,size);
}

}
