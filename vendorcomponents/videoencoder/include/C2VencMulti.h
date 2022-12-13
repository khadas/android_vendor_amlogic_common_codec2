#ifndef ANDROID_C2_VENC_MULTI_H__
#define ANDROID_C2_VENC_MULTI_H__

#include <map>
#include <inttypes.h>
#include <utils/Vector.h>
#include <C2VencComponent.h>
#include "vp_multi_codec_1_0.h"



namespace android {

typedef vl_codec_handle_t (*fn_vl_multi_encoder_init)(vl_codec_id_t codec_id,
                                                             vl_encode_info_t encode_info,
                                                             qp_param_t* qp_tbl);
typedef encoding_metadata_t (*fn_vl_multi_generate_header)(vl_codec_handle_t codec_handle,
                                                                  unsigned char *pHeader,
                                                                  unsigned int *pLength);
typedef encoding_metadata_t (*fn_vl_multi_encode_frame)(vl_codec_handle_t handle,
                                                              vl_frame_type_t type,
                                                              unsigned char* out,
                                                              vl_buffer_info_t *in_buffer_info,
                                                              vl_buffer_info_t *ret_buffer_info);
typedef int (*fn_vl_multi_change_bitrate)(vl_codec_handle_t handle, int bitRate);
typedef int (*fn_vl_multi_encoder_destroy)(vl_codec_handle_t handle);

class C2VencMulti:public C2VencComponent {
public:
    class IntfImpl;

    C2VencMulti(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);

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
    virtual ~C2VencMulti();

    //static
    static std::atomic<int32_t> sConcurrentInstances;
    // static
    static std::shared_ptr<C2Component> create(
            char *name, c2_node_id_t id, const std::shared_ptr<C2VencMulti::IntfImpl>& helper);

    // The pointer of component interface implementation.
private:
    c2_status_t genVuiParam(int32_t *primaries,int32_t *transfer,int32_t *matrixCoeffs,bool *range);
    bool codecTypeTrans(uint32_t inputCodec,vl_img_format_t *pOutputCodec);
    bool codec2TypeTrans(ColorFmt inputFmt,vl_img_format_t *pOutputCodec);
    void codec2ProfileTrans(int *profile);
    void codec2InitQpTbl(qp_param_t *qp_tbl);
    c2_status_t getQp(int32_t *i_qp_max,int32_t *i_qp_min,int32_t *p_qp_max,int32_t *p_qp_min,int32_t *b_qp_max,int32_t *b_qp_min);
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
    std::shared_ptr<C2VencCanvasMode::input> mVencCanvasMode;
    std::shared_ptr<C2PrependHeaderModeSetting> mPrependHeader;
    std::shared_ptr<C2StreamTemporalLayeringTuning::output> mLayerCount;

    std::shared_ptr<IntfImpl> mIntfImpl;
    fn_vl_multi_encoder_init mInitFunc;
    fn_vl_multi_generate_header mEncHeaderFunc;
    fn_vl_multi_encode_frame mEncFrameFunc;
    fn_vl_multi_change_bitrate mEncBitrateChangeFunc;
    fn_vl_multi_encoder_destroy mDestroyFunc;

    vl_codec_handle_t mCodecHandle;
    uint32_t mIDRInterval;
    uint32_t mBitrateBak;
    vl_codec_id_t mCodecID;
};

}



#endif


