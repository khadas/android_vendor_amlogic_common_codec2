#ifndef ANDROID_C2_VENC_HCODEC_H__
#define ANDROID_C2_VENC_HCODEC_H__

#include <map>
#include <inttypes.h>
#include <utils/Vector.h>
#include <C2VencComponent.h>
#include "vp_hevc_codec_1_0.h"


namespace android {

typedef vl_codec_handle_t (*fn_hevc_video_encoder_init)(vl_codec_id_t codec_id, vl_init_params_t initParam);
typedef int (*fn_hevc_video_encode_header)(vl_codec_handle_t codec_handle, int in_size, unsigned char *out);
typedef int (*fn_hevc_video_encoder_encode)(vl_codec_handle_t codec_handle, vl_frame_info_t frameinfo,unsigned int outputBufferLen, unsigned char *out,vl_frame_type_t *frameType);
typedef int (*fn_hevc_video_encoder_destory)(vl_codec_handle_t codec_handle);

class C2VencW420:public C2VencComponent {
public:
    class IntfImpl;

    C2VencW420(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);

    // From SimpleC2Component
    bool LoadModule() override;
    c2_status_t Init() override;
    c2_status_t ProcessOneFrame(InputFrameInfo_t InputFrameInfo,OutputFrameInfo_t *pOutFrameInfo) override;
    c2_status_t GenerateHeader(uint8_t *pHeaderData,uint32_t *pSize) override;
    void Close() override;
    void getResolution(int *pWidth,int *pHeight) override;

//protected:
    virtual ~C2VencW420();

    // The pointer of component interface implementation.
private:
    c2_status_t genVuiParam(int32_t *primaries,int32_t *transfer,int32_t *matrixCoeffs,bool *range);
    bool codecTypeTrans(uint32_t inputCodec,vl_img_format_t *pOutputCodec);
    bool codec2TypeTrans(ColorFmt inputFmt,vl_img_format_t *pOutputCodec);
    c2_status_t getQp(int32_t *i_qp_max,int32_t *i_qp_min,int32_t *p_qp_max,int32_t *p_qp_min);
    void ParseGop(const C2StreamGopTuning::output &gop,uint32_t *syncInterval, uint32_t *iInterval, uint32_t *maxBframes);
    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    std::shared_ptr<C2StreamIntraRefreshTuning::output> mIntraRefresh;
    std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestSync;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
    std::shared_ptr<C2StreamGopTuning::output> mGop;
    std::shared_ptr<C2StreamPixelFormatInfo::input> mPixelFormat;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mCodedColorAspects;
    std::shared_ptr<C2StreamProfileLevelInfo::output> mProfileLevel;
    std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> mSyncFramePeriod;

    std::shared_ptr<IntfImpl> mIntfImpl;

    fn_hevc_video_encoder_init mInitFunc;
    fn_hevc_video_encode_header mEncHeaderFunc;
    fn_hevc_video_encoder_encode mEncFrameFunc;
    fn_hevc_video_encoder_destory mDestroyFunc;
    vl_codec_handle_t mCodecHandle;
    uint32_t mIDRInterval;
    //uint32_t mIInterval;
    //uint32_t mBframes;
};

}



#endif

