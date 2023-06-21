#ifndef ANDROID_C2_VENC_W420_NEW_H__
#define ANDROID_C2_VENC_W420_NEW_H__

#include <map>
#include <inttypes.h>
#include <utils/Vector.h>
#include <C2VencComponent.h>
#include "vp_hevc_codec_1_0.h"


namespace android {

typedef vl_codec_handle_hevc_t (*fn_hevc_video_encoder_init)(vl_codec_id_hevc_t codec_id,vl_encode_info_hevc_t encode_info,qp_param_hevc_t* qp_tbl);
typedef encoding_metadata_hevc_t (*fn_hevc_video_encode_header)(vl_codec_handle_hevc_t codec_handle,unsigned char *pHeader,unsigned int *pLength);
typedef encoding_metadata_hevc_t (*fn_hevc_video_encoder_encode)(vl_codec_handle_hevc_t handle,vl_frame_type_hevc_t frame_type,
                                                    unsigned char *out,vl_buffer_info_hevc_t *in_buffer_info);
typedef int (*fn_vl_change_bitrate)(vl_codec_handle_hevc_t codec_handle,int bitRate);
typedef int (*fn_vl_change_framerate_hevc)(vl_codec_handle_hevc_t codec_handle,int frameRate,int bitRate);

typedef int (*fn_vl_video_encoder_getavgqp)(vl_codec_handle_hevc_t codec_handle,int *avg_qp);

typedef int (*fn_hevc_video_encoder_destroy)(vl_codec_handle_hevc_t handle);

class C2VencW420New:public C2VencComponent {
public:
    class IntfImpl;

    C2VencW420New(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);

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
    virtual ~C2VencW420New();
    //static
    static std::atomic<int32_t> sConcurrentInstances;
    static uint32_t mInstanceID;
    // static
    static std::shared_ptr<C2Component> create(
            char *name, c2_node_id_t id, const std::shared_ptr<C2VencW420New::IntfImpl>& helper);

    // The pointer of component interface implementation.
private:
    c2_status_t genVuiParam(int32_t *primaries,int32_t *transfer,int32_t *matrixCoeffs,bool *range);
    bool codecTypeTrans(uint32_t inputCodec,vl_img_format_hevc_t *pOutputCodec);
    bool codec2TypeTrans(ColorFmt inputFmt,vl_img_format_hevc_t *pOutputCodec);
    c2_status_t getQp(int32_t *i_qp_max,int32_t *i_qp_min,int32_t *p_qp_max,int32_t *p_qp_min);
    void ParseGop(const C2StreamGopTuning::output &gop,uint32_t *syncInterval, uint32_t *iInterval, uint32_t *maxBframes);
    int getFrameRate(int32_t frameIndex,int64_t timestamp);
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

    std::shared_ptr<C2AndroidStreamAverageBlockQuantizationInfo::output> mAverageBlockQuantization;
    std::shared_ptr<C2StreamPictureTypeInfo::output> mPictureType;
    std::shared_ptr<IntfImpl> mIntfImpl;

    fn_hevc_video_encoder_init mInitFunc;
    fn_hevc_video_encode_header mEncHeaderFunc;
    fn_hevc_video_encoder_encode mEncFrameFunc;
    fn_vl_video_encoder_getavgqp mEncFrameQpFunc;
    fn_vl_change_bitrate mEncBitrateChangeFunc;
    fn_vl_change_framerate_hevc mEncFrameRateChangeFunc;
    fn_hevc_video_encoder_destroy mDestroyFunc;
    vl_codec_handle_hevc_t mCodecHandle;
    int32_t mIDRInterval;
    uint32_t mBitrateBak;
    //uint64_t mtimeStampBak;
    //uint32_t curFrameRateBak;
    uint64_t mtimeStampBak;
    uint32_t mFrameRateValue;
    //uint32_t mIInterval;
    //uint32_t mBframes;
};

}



#endif

