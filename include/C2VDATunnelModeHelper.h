#ifndef _C2VDA_TUNNELMODE_HELPER_H_
#define _C2VDA_TUNNELMODE_HELPER_H_

#include <mutex>

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>

#include <C2VDAComponent.h>

#include <C2VDATunnelBufferUtil.h>
#include <VideoTunnelRendererWraper.h>

namespace android {

class C2VDAComponent::TunnelModeHelper {
public:
    TunnelModeHelper(C2VDAComponent* comp, bool secure);
    virtual ~TunnelModeHelper();

    bool start();
    bool stop();

    static int fillVideoFrameCallback2(void* obj, void* args);
    int postFillVideoFrameTunnelMode2(int medafd, bool rendered);
    void onFillVideoFrameTunnelMode2(int medafd, bool rendered);

    static int notifyTunnelRenderTimeCallback(void* obj, void* args);
    int postNotifyRenderTimeTunnelMode(struct VideoTunnelRendererWraper::renderTime* rendertime);
    void onNotifyRenderTimeTunnelMode(struct VideoTunnelRendererWraper::renderTime rendertime);

    c2_status_t sendVideoFrameToVideoTunnel(int32_t pictureBufferId, int64_t bitstreamId);
    c2_status_t flush();
    c2_status_t sendOutputBufferToWorkTunnel(struct VideoTunnelRendererWraper::renderTime* rendertime);
    c2_status_t storeAbandonedFrame(int64_t timeus);
    c2_status_t allocateTunnelBufferFromBlockPool(const media::Size& size, uint32_t pixelFormat);
    c2_status_t freeTunnelBuffers();
    c2_status_t videoResolutionChangeTunnel();

private:
    bool checkReallocOutputBuffer(VideoFormat video_format_old,VideoFormat video_format_new, bool *sizeChanged, bool *bufferNumLarged);
    void appendTunnelOutputBuffer(int fd, uint32_t blockId);
    uint64_t getPlatformUsage();

    C2VDAComponent* mComp;
    TunnelBufferUtil* mTunnelBufferUtil;

    C2VDAComponent::IntfImpl* mIntfImpl;
    //C2VDAComponent::MetaDataUtil* mMetaDataUtil;
    scoped_refptr<::base::SingleThreadTaskRunner> mTaskRunner;
    VideoTunnelRendererWraper* mVideoTunnelRenderer;

    bool mSecure;
    int32_t mTunnelId;
    int32_t mSyncId;
    native_handle_t* mTunnelHandle;

    std::vector<struct fillVideoFrame2> mFillVideoFrameQueue;
    std::vector<int64_t> mTunnelAbandonMediaTimeQueue;
    std::deque<int64_t> mTunnelRenderMediaTimeQueue;

    uint32_t mOutBufferCount;
    uint32_t mPixelFormat;
    bool mReallocWhenResChange;
};
}
#endif
