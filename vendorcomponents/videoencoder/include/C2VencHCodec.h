#ifndef ANDROID_C2_VENC_HCODEC_H__
#define ANDROID_C2_VENC_HCODEC_H__

#include <map>
#include <inttypes.h>
#include <utils/Vector.h>
#include <C2VendorProperty.h>
#include <c2logdebug.h>
#include <C2VencComponent.h>
#include "vpcodec_1_0.h"


namespace android {


typedef vl_codec_handle_t (*fn_vl_video_encoder_init)(vl_codec_id_t codec_id, vl_init_params_t initParam);
typedef int (*fn_vl_video_encode_header)(vl_codec_handle_t codec_handle, vl_vui_params_t vui, int in_size, unsigned char *out);
typedef vl_enc_result_e (*fn_vl_video_encoder_encode_frame)(vl_codec_handle_t codec_handle, vl_frame_info_t frame_info, unsigned char *out,int *out_size,vl_frame_type_t *frame_type);
typedef vl_enc_result_e (*fn_vl_video_encoder_getavgqp)(vl_codec_handle_t codec_handle, float *avg_qp);

typedef int (*fn_vl_video_encoder_destroy)(vl_codec_handle_t handle);

class C2VencHCodec:public C2VencComponent {
public:
    class IntfImpl;

    C2VencHCodec(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);

    // From SimpleC2Component
    bool LoadModule() override;
    c2_status_t Init() override;
    c2_status_t ProcessOneFrame(InputFrameInfo_t InputFrameInfo,OutputFrameInfo_t *pOutFrameInfo) override;
    c2_status_t GenerateHeader(uint8_t *pHeaderData,uint32_t *pSize) override;
    void Close() override;
    void getResolution(int *pWidth,int *pHeight) override;
    void getCodecDumpFileName(std::string &strName,DumpFileType_e type) override;
    bool isSupportDMA() override;
    bool isSupportCanvas() override;

//protected:
    virtual ~C2VencHCodec();

    //static
    static std::atomic<int32_t> sConcurrentInstances;
    static uint32_t mInstanceID;
    // static
    static std::shared_ptr<C2Component> create(
            char *name, c2_node_id_t id, const std::shared_ptr<C2VencHCodec::IntfImpl>& helper);

    // The pointer of component interface implementation.
private:
    c2_status_t genVuiParam(int32_t *primaries,int32_t *transfer,int32_t *matrixCoeffs,bool *range);
    bool codecTypeTrans(uint32_t inputCodec,vl_img_format_t *pOutputCodec);
    bool codec2TypeTrans(ColorFmt inputFmt,vl_img_format_t *pOutputCodec);
    void codec2ProfileLevelTrans(vl_h_enc_profile_e *profile,vl_h_enc_level_e *level);
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
    std::shared_ptr<C2PrependHeaderModeSetting> mPrependHeader;
    std::shared_ptr<C2VencCanvasMode::input> mVencCanvasMode;
    std::shared_ptr<C2AndroidStreamAverageBlockQuantizationInfo::output> mAverageBlockQuantization;
    std::shared_ptr<C2StreamPictureTypeInfo::output> mPictureType;

    std::shared_ptr<IntfImpl> mIntfImpl;
    fn_vl_video_encoder_init mInitFunc;
    fn_vl_video_encode_header mEncHeaderFunc;
    fn_vl_video_encoder_encode_frame mEncFrameFunc;
    fn_vl_video_encoder_getavgqp mEncFrameQpFunc;
    fn_vl_video_encoder_destroy mDestroyFunc;

    vl_codec_handle_t mCodecHandle;
    uint32_t mIDRInterval;
    uint64_t mtimeStampBak;
    uint32_t mFrameRateValue;
    uint32_t mBitrateBk;
    uint32_t mBitRate;
    //uint32_t mIInterval;
    //uint32_t mBframes;
};

}



#endif

