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
    void videoSyncQueueVideoFrame(int64_t timestampUs, uint32_t size);
    VideoTunnelRendererBase* getTunnelRenderer() { return mVideoTunnelRenderer;  };

private:
    VideoTunnelRendererBase* mVideoTunnelRenderer;
};

}

#endif
