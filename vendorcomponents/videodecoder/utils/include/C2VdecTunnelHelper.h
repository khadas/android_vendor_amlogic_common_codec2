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

#ifndef _C2Vdec_Tunnel_HELPER_H_
#define _C2Vdec_Tunnel_HELPER_H_

#include <mutex>

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>

#include <C2VdecComponent.h>

#include <C2VdecBlockPoolUtil.h>
#include <VideoTunnelRendererWraper.h>

namespace android {

class C2VdecComponent::TunnelHelper : public IC2Observer {
public:
    TunnelHelper(bool secure);
    virtual ~TunnelHelper();

    c2_status_t setComponent(std::shared_ptr<C2VdecComponent> comp);
    c2_status_t start();
    c2_status_t stop();
    c2_status_t flush();
    c2_status_t sendVideoFrameToVideoTunnel(int32_t pictureBufferId, int64_t bitstreamId,uint64_t timestamp);
    c2_status_t storeAbandonedFrame(int64_t timeus);
    c2_status_t allocTunnelBuffersAndSendToDecoder(const media::Size& size, uint32_t pixelFormat);
    c2_status_t videoResolutionChangeTunnel();
    void onAndroidVideoPeek();
    VideoTunnelRendererBase* getTunnelRender() { return mVideoTunnelRenderer->getTunnelRenderer();}
    c2_status_t fastHandleWorkTunnel(int64_t bitstreamId, int32_t pictureBufferId);
    c2_status_t fastHandleOutBufferTunnel(uint64_t timestamp, int32_t pictureBufferId);
    void configureEsModeHwAvsyncId(int32_t            avSyncId);
    void videoSyncQueueVideoFrame(int64_t timestampUs, uint32_t size);
private:
    static int fillVideoFrameCallback2(void* obj, void* args);
    int postFillVideoFrameTunnel2(int dmafd, bool rendered);
    void onFillVideoFrameTunnel2(int dmafd, bool rendered);

    static int notifyTunnelRenderTimeCallback(void* obj, void* args);
    int postNotifyRenderTimeTunnel(struct renderTime* rendertime);
    void onNotifyRenderTimeTunnel(struct renderTime rendertime);

    static int notifyTunnelEventCallback(void* obj, void* args);
    int postNotifyTunnelEvent(struct tunnelEventParam* param);
    void onNotifyTunnelEvent(struct tunnelEventParam param);

    c2_status_t sendOutputBufferToWorkTunnel(struct renderTime* rendertime);
    bool checkReallocOutputBuffer(VideoFormat video_format_old,VideoFormat video_format_new, bool *sizeChanged, bool *bufferNumEnlarged);
    void appendTunnelOutputBuffer(std::shared_ptr<C2GraphicBlock> block, int fd, uint32_t blockId, uint32_t poolId);
    uint64_t getPlatformUsage();
    c2_status_t allocTunnelBuffer(const media::Size& size, uint32_t pixelFormat, int* pFd);
    void allocTunnelBufferAndSendToDecoder(const media::Size& size, uint32_t pixelFormat, int index);
    c2_status_t resetBlockPoolBuffers();
    bool isInResolutionChanging();

    std::weak_ptr<C2VdecComponent> mComp;
    std::weak_ptr<C2VdecComponent::IntfImpl> mIntfImpl;
    std::shared_ptr<VideoTunnelRendererWraper> mVideoTunnelRenderer;
    std::weak_ptr<C2VdecBlockPoolUtil> mBlockPoolUtil;
    std::weak_ptr<C2VdecComponent::DeviceUtil> mDeviceUtil;

    bool mSecure;
    int32_t mTunnelId;
    int32_t mSyncId;
    native_handle_t* mTunnelHandle;

    struct TunnelFdInfo {
      TunnelFdInfo(int fd, int blockid):
        mFd(fd),
        mBlockId(blockid) {}
      ~TunnelFdInfo() {}
      int mFd;
      int mBlockId;
    };

    std::vector<struct fillVideoFrame2> mFillVideoFrameQueue;
    std::vector<int64_t> mTunnelAbandonMediaTimeQueue;
    std::map<int, TunnelFdInfo> mOutBufferFdMap;

    uint32_t mOutBufferCount;
    uint32_t mPixelFormat;
    bool mReallocWhenResChange;

};
}
#endif
