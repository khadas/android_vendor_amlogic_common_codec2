/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
#ifndef VIDEO_TUNNEL_RENDERER_WRAPER_H_
#define VIDEO_TUNNEL_RENDERER_WRAPER_H_

#include <cutils/native_handle.h>
#include <sys/types.h>
#include <VideoTunnelRendererBase.h>

namespace android {

class VideoTunnelRendererWraper
{
public:
    /* event type*/
    enum {
        CB_EVENT_UNDERFLOW = 1,
    };

public:
    VideoTunnelRendererWraper(bool secure = false);
    ~VideoTunnelRendererWraper();
    bool init(int hwsyncid);
    bool start();
    bool stop();
    bool flush();

    int getTunnelId();
    bool sendVideoFrame(const int metafd, int64_t timestampNs, bool renderAtonce=false);
    bool peekFirstFrame();
    int regFillVideoFrameCallBack(callbackFunc funs, void* obj);
    int regNotifyTunnelRenderTimeCallBack(callbackFunc funs, void* obj);
    int regNotifyEventCallBack(callbackFunc funs, void* obj);
    bool setFrameRate(int32_t framerate);
    VideoTunnelRendererBase* getTunnelRenderer() { return mVideoTunnelRenderer;  };

private:
    VideoTunnelRendererBase* mVideoTunnelRenderer;
};

}

#endif
