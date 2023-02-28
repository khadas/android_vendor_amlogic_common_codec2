/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
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

class C2VdecComponent::TunnelHelper {
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
    c2_status_t fastHandleWorkAndOutBufferTunnel(bool input, int64_t bitstreamId, int32_t pictureBufferId);
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

    bool mAndroidPeekFrameReady;
};
}
#endif
