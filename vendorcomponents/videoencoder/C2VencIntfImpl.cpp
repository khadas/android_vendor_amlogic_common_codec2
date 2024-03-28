#define LOG_TAG "C2VencIntfImpl"
#include <utils/Log.h>
#include <utils/misc.h>

#include <algorithm>

#include <media/hardware/VideoAPI.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/foundation/AUtils.h>

#include <C2Debug.h>
#include <C2Config.h>
#include <Codec2Mapper.h>
#include <C2PlatformSupport.h>
#include <Codec2BufferUtils.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>
#include "C2VencIntfImpl.h"
#include <dlfcn.h>


namespace android {


stPicSize C2VencComp::IntfImpl::mMaxSize;


const C2String kComponentLoadMediaProcessLibrary = "lib_encoder_media_process.so";


#define C2Venc_IntfImpl_Log(level, fmt, str...) CODEC2_VENC_LOG(level, "[##%d##]"#fmt, mInstanceID, ##str)

#define SUPPORT_DMA   1  //support dma mode or not
constexpr static size_t kLinearBufferSize = 5 * 1024 * 1024;

#define ENC_ENABLE_ROI_FEATURE      0x1
#define ENC_ENABLE_PARA_UPDATE      0x2 //enable dynamic settings
#define ENC_ENABLE_LONG_TERM_REF    0x80

#define MAX_INPUT_BUFFER_HEADERS 4
#define MAX_CONVERSION_BUFFERS   4
#define CODEC_MAX_CORES          4
#define LEN_STATUS_BUFFER        (10  * 1024)
#define MAX_VBV_BUFF_SIZE        (120 * 16384)
#define MAX_NUM_IO_BUFS           3

#define DEFAULT_MAX_REF_FRM         2
#define DEFAULT_MAX_REORDER_FRM     0
#define DEFAULT_QP_MIN              10
#define DEFAULT_QP_MAX              40
#define DEFAULT_MAX_BITRATE         240000000
#define DEFAULT_MAX_SRCH_RANGE_X    256
#define DEFAULT_MAX_SRCH_RANGE_Y    256
#define DEFAULT_MAX_FRAMERATE       120000
#define DEFAULT_NUM_CORES           1
#define DEFAULT_NUM_CORES_PRE_ENC   0
#define DEFAULT_FPS                 30
#define DEFAULT_ENC_SPEED           IVE_NORMAL

#define DEFAULT_MEM_REC_CNT         0
#define DEFAULT_RECON_ENABLE        0
#define DEFAULT_CHKSUM_ENABLE       0
#define DEFAULT_START_FRM           0
#define DEFAULT_NUM_FRMS            0xFFFFFFFF
#define DEFAULT_INP_COLOR_FORMAT       IV_YUV_420SP_VU
#define DEFAULT_RECON_COLOR_FORMAT     IV_YUV_420P
#define DEFAULT_LOOPBACK            0
#define DEFAULT_SRC_FRAME_RATE      30
#define DEFAULT_TGT_FRAME_RATE      30
#define DEFAULT_MAX_WD              1920
#define DEFAULT_MAX_HT              1920
#define DEFAULT_MAX_LEVEL           41
#define DEFAULT_STRIDE              0
#define DEFAULT_WD                  1280
#define DEFAULT_HT                  720
#define DEFAULT_PSNR_ENABLE         0
#define DEFAULT_ME_SPEED            100
#define DEFAULT_ENABLE_FAST_SAD     0
#define DEFAULT_ENABLE_ALT_REF      0
#define DEFAULT_RC_MODE             IVE_RC_STORAGE
#define DEFAULT_BITRATE             6000000
#define DEFAULT_I_QP                22
#define DEFAULT_I_QP_MAX            DEFAULT_QP_MAX
#define DEFAULT_I_QP_MIN            DEFAULT_QP_MIN
#define DEFAULT_P_QP                28
#define DEFAULT_P_QP_MAX            DEFAULT_QP_MAX
#define DEFAULT_P_QP_MIN            DEFAULT_QP_MIN
#define DEFAULT_B_QP                22
#define DEFAULT_B_QP_MAX            DEFAULT_QP_MAX
#define DEFAULT_B_QP_MIN            DEFAULT_QP_MIN
#define DEFAULT_AIR                 IVE_AIR_MODE_NONE
#define DEFAULT_AIR_REFRESH_PERIOD  30
#define DEFAULT_SRCH_RNG_X          64
#define DEFAULT_SRCH_RNG_Y          48
#define DEFAULT_I_INTERVAL          30
#define DEFAULT_IDR_INTERVAL        1000
#define DEFAULT_B_FRAMES            0
#define DEFAULT_DISABLE_DEBLK_LEVEL 0
#define DEFAULT_HPEL                1
#define DEFAULT_QPEL                1
#define DEFAULT_I4                  1
#define DEFAULT_EPROFILE            IV_PROFILE_BASE
#define DEFAULT_ENTROPY_MODE        0
#define DEFAULT_SLICE_MODE          IVE_SLICE_MODE_NONE
#define DEFAULT_SLICE_PARAM         256
#define DEFAULT_ARCH                ARCH_ARM_A9Q
#define DEFAULT_SOC                 SOC_GENERIC
#define DEFAULT_INTRA4x4            0
#define DEFAULT_CONSTRAINED_INTRA   0

#define CODEC_QP_MIN                4
#define CODEC_QP_MAX                51

#define HCODEC_MAX_B_FRAMES 1

#define ENC_PROFILE_AUTO        0
#define ENC_PROFILE_BASELINE    1
#define ENC_PROFILE_MAIN        2
#define ENC_PROFILE_HIGH        3


int ColorFormatSupprt[] = {
    HAL_PIXEL_FORMAT_YCBCR_420_888,
    HAL_PIXEL_FORMAT_YCRCB_420_SP,
    HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED //color from surface!!!
};


bool C2VencComp::IntfImpl::Load() {
    mLibHandle = dlopen(kComponentLoadMediaProcessLibrary.c_str(), RTLD_NOW | RTLD_NODELETE);
    if (mLibHandle == nullptr) {
        ALOGD("Could not dlopen %s: %s", kComponentLoadMediaProcessLibrary.c_str(), dlerror());
        return false;
    }
    ALOGE("IntfImpl::mLibHandle:%p",mLibHandle);
    mCreateMethod = (C2VencParamCreateInstance)dlsym(
                mLibHandle, "VencParamGetInstance");

    mDestroyMethod = (C2VencParamDestroyInstance)dlsym(
                mLibHandle, "VencParamDelInstance");

    if (!mCreateMethod || !mDestroyMethod) {
        ALOGE("load library failed,mCreateMethod:%p,mDestroyMethod:%p",mCreateMethod,mDestroyMethod);
        return false;
    }

    if (mCreateMethod) {
        mAmlVencParam = mCreateMethod();
        if (NULL == mAmlVencParam) {
            ALOGE("mAmlVencParam is null");
            return false;
        }
    }
    else {
        ALOGE("mCreateMethod is null");
        return false;
    }

    if (!mDestroyMethod) {
        ALOGE("mDestroyMethod is null");
        return false;
    }
    return true;
}


void C2VencComp::IntfImpl::unLoad() {
    if (mDestroyMethod) {
        ALOGE("Destroy mAmlVencParam");
        mDestroyMethod(mAmlVencParam);
    }

    if (mLibHandle) {
        ALOGE("Unloading dll");
        dlclose(mLibHandle);
    }
}



C2VencComp::IntfImpl::IntfImpl(C2String name,C2String mimetype,const std::shared_ptr<C2ReflectorHelper> &helper)
    : SimpleInterface<void>::BaseParams(
            helper,
            name,
            C2Component::KIND_ENCODER,
            C2Component::DOMAIN_VIDEO,
            mimetype),
      mAmlVencParam(NULL),
      mLibHandle(NULL),
      mCreateMethod(NULL),
      mDestroyMethod(NULL) {

    //mAmlVencParam = IAmlVencParam::GetInstance();
    Load();

    if (NULL == mAmlVencParam) {
        ALOGE("mAmlVencParam is NULL!!!!");
        return;
    }

    mAmlVencParam->RegisterChangeNotify(C2VencComp::IntfImpl::VencParamChangeListener,this);
    ALOGE("mimetype:%s",mimetype.c_str());
    mAmlVencParam->SetCodecType((std::string::npos != mimetype.find("hevc")) ? H265 : H264);

    ALOGD("C2VencMulti::IntfImpl constructor!");
    setDerivedInstance(this);
    addParameter(
                DefineParam(mUsage, C2_PARAMKEY_INPUT_STREAM_USAGE)
                .withConstValue(new C2StreamUsageTuning::input(
                        0u, mAmlVencParam->GetIsSupportDMAMode() ? 0 : (uint64_t)C2MemoryUsage::CPU_READ))
                .build());

    addParameter(
            DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
            .withConstValue(new C2ComponentAttributesSetting(
                C2Component::ATTRIB_IS_TEMPORAL))
            .build());

    onSizeParam(mimetype);

    addParameter(
            DefineParam(mPictureQuantization, C2_PARAMKEY_PICTURE_QUANTIZATION)
            .withDefault(C2StreamPictureQuantizationTuning::output::AllocShared(
                    0 /* flexCount */, 0u /* stream */))
            .withFields({C2F(mPictureQuantization, m.values[0].type_).oneOf(
                            {C2Config::picture_type_t(I_FRAME),
                              C2Config::picture_type_t(P_FRAME),
                              C2Config::picture_type_t(B_FRAME)}),
                         C2F(mPictureQuantization, m.values[0].min).any(),
                         C2F(mPictureQuantization, m.values[0].max).any()})
            .withSetter(PictureQuantizationSetter)
            .build());

    addParameter(
            DefineParam(mGop, C2_PARAMKEY_GOP)
            .withDefault(C2StreamGopTuning::output::AllocShared(
                    0 /* flexCount */, 0u /* stream */))
            .withFields({C2F(mGop, m.values[0].type_).any(),
                         C2F(mGop, m.values[0].count).any()})
            .withSetter(GopSetter)
            .build());

    addParameter(
            DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
            .withDefault(new C2StreamFrameRateInfo::output(0u, 1.))
            // TODO: More restriction?
            .withFields({C2F(mFrameRate, value).greaterThan(0.)})
            .withSetter(FramerateSetter/*Setter<decltype(*mFrameRate)>::StrictValueWithNoDeps*/)
            .build());

    addParameter(
            DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
            .withDefault(new C2StreamBitrateInfo::output(0u, 64000))
            .withFields({C2F(mBitrate, value).inRange(4096, 12000000)})
            .withSetter(BitrateSetter)
            .build());

    addParameter(
            DefineParam(mIntraRefresh, C2_PARAMKEY_INTRA_REFRESH)
            .withDefault(new C2StreamIntraRefreshTuning::output(
                    0u, C2Config::INTRA_REFRESH_DISABLED, 0.))
            .withFields({
                C2F(mIntraRefresh, mode).oneOf({
                    C2Config::INTRA_REFRESH_DISABLED, C2Config::INTRA_REFRESH_ARBITRARY }),
                C2F(mIntraRefresh, period).any()
            })
            .withSetter(IntraRefreshSetter)
            .build());

    if (mimetype == MEDIA_MIMETYPE_VIDEO_AVC) {
        onAvcProfileLevelParam();
    }
    else {
        onHevcProfileLevelParam();
    }

    addParameter(
            DefineParam(mPictureType, C2_PARAMKEY_PICTURE_TYPE)
            .withDefault(new C2StreamPictureTypeInfo::output(0u,C2Config::picture_type_t(SYNC_FRAME)))
            .withFields({C2F(mPictureType, value).oneOf({C2Config::SYNC_FRAME, C2Config::I_FRAME, C2Config::P_FRAME, C2Config::B_FRAME })})
            .withSetter(Setter<decltype(*mPictureType)>::StrictValueWithNoDeps)
            .build());

    addParameter(
            DefineParam(mAverageBlockQuantization, C2_PARAMKEY_AVERAGE_QP)
            .withDefault(new C2AndroidStreamAverageBlockQuantizationInfo::output(0u, 0))
            .withFields({C2F(mAverageBlockQuantization, value).any()})
            .withSetter(Setter<decltype(*mAverageBlockQuantization)>::StrictValueWithNoDeps)
            .build());

    addParameter(
            DefineParam(mRequestSync, C2_PARAMKEY_REQUEST_SYNC_FRAME)
            .withDefault(new C2StreamRequestSyncFrameTuning::output(0u, C2_FALSE))
            .withFields({C2F(mRequestSync, value).oneOf({ C2_FALSE, C2_TRUE }) })
            .withSetter(Setter<decltype(*mRequestSync)>::NonStrictValueWithNoDeps)
            .build());

    addParameter(
            DefineParam(mSyncFramePeriod, C2_PARAMKEY_SYNC_FRAME_INTERVAL)
            .withDefault(new C2StreamSyncFrameIntervalTuning::output(0u, 1000000))
            .withFields({C2F(mSyncFramePeriod, value).any()})
            .withSetter(Setter<decltype(*mSyncFramePeriod)>::StrictValueWithNoDeps)
            .build());

    addParameter(
            DefineParam(mColorAspects, C2_PARAMKEY_COLOR_ASPECTS)
            .withDefault(new C2StreamColorAspectsInfo::input(
                    0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                    C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
            .withFields({
                C2F(mColorAspects, range).inRange(
                            C2Color::RANGE_UNSPECIFIED,     C2Color::RANGE_OTHER),
                C2F(mColorAspects, primaries).inRange(
                            C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                C2F(mColorAspects, transfer).inRange(
                            C2Color::TRANSFER_UNSPECIFIED,  C2Color::TRANSFER_OTHER),
                C2F(mColorAspects, matrix).inRange(
                            C2Color::MATRIX_UNSPECIFIED,    C2Color::MATRIX_OTHER)
            })
            .withSetter(ColorAspectsSetter)
            .build());

    addParameter(
            DefineParam(mCodedColorAspects, C2_PARAMKEY_VUI_COLOR_ASPECTS)
            .withDefault(new C2StreamColorAspectsInfo::output(
                    0u, C2Color::RANGE_LIMITED, C2Color::PRIMARIES_UNSPECIFIED,
                    C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
            .withFields({
                C2F(mCodedColorAspects, range).inRange(
                            C2Color::RANGE_UNSPECIFIED,     C2Color::RANGE_OTHER),
                C2F(mCodedColorAspects, primaries).inRange(
                            C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                C2F(mCodedColorAspects, transfer).inRange(
                            C2Color::TRANSFER_UNSPECIFIED,  C2Color::TRANSFER_OTHER),
                C2F(mCodedColorAspects, matrix).inRange(
                            C2Color::MATRIX_UNSPECIFIED,    C2Color::MATRIX_OTHER)
            })
            .withSetter(CodedColorAspectsSetter, mColorAspects)
            .build());

    // TODO: support more formats?
    std::vector<uint32_t> pixelFormats;
    for (int i = 0;i < sizeof(ColorFormatSupprt) / sizeof(int);i++)
    {
        pixelFormats.push_back(ColorFormatSupprt[i]);
    }

    addParameter(
            DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
            .withDefault(new C2StreamPixelFormatInfo::input(
                              0u, HAL_PIXEL_FORMAT_YCRCB_420_SP))
            .withFields({C2F(mPixelFormat, value).oneOf(pixelFormats)})
            .withSetter((Setter<decltype(*mPixelFormat)>::StrictValueWithNoDeps))
            .build());

    addParameter(
        DefineParam(mVencCanvasMode, C2_PARAMKEY_VENDOR_VENC_CANVAS_MODE)
                .withDefault(new C2VencCanvasMode::input(0))
                .withFields({C2F(mVencCanvasMode, value).any()})
                .withSetter(Setter<decltype(*mVencCanvasMode)>::StrictValueWithNoDeps)
        .build());

    addParameter(
            DefineParam(mPrependHeader, C2_PARAMKEY_PREPEND_HEADER_MODE)
            .withConstValue(new C2PrependHeaderModeSetting(
                C2Config::PREPEND_HEADER_TO_ALL_SYNC))
            .build());

    addParameter(
            DefineParam(mMaxInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
            .withDefault(new C2StreamMaxBufferSizeInfo::input(0u, mAmlVencParam->GetMaxOutputBufferSize()))
            .withFields({C2F(mMaxInputSize, value).any()})
            .calculatedAs(MaxSizeCalculator)
            .build());

    onSVCParam();


}


C2VencComp::IntfImpl::~IntfImpl() {
    //IAmlVencParam::DelInstance(mAmlVencParam);
    unLoad();
}


IAmlVencParam *C2VencComp::IntfImpl::GetVencParam() {
    return mAmlVencParam;
};


void C2VencComp::IntfImpl::onSizeParam(C2String mimetype) {
    stPicSize MinSize;
    //stPicSize MaxSize;
    stPicAlign align;
    memset(&MinSize,0,sizeof(MinSize));
    memset(&mMaxSize,0,sizeof(mMaxSize));
    memset(&align,0,sizeof(align));

    mAmlVencParam->GetSizeLimit(MinSize, mMaxSize, align);
    addParameter(
            DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
            .withDefault(new C2StreamPictureSizeInfo::input(0u, MinSize.width, MinSize.height))
            .withFields({
                C2F(mSize, width).inRange(MinSize.width, mMaxSize.width, align.width/*(mimetype == MEDIA_MIMETYPE_VIDEO_AVC) ? 2 : 8*/),
                C2F(mSize, height).inRange(MinSize.height, mMaxSize.width, align.height),
            })
            .withSetter(SizeSetter)
            .build());
}


void C2VencComp::IntfImpl::onSVCParam() {
    stSVCInfo SvcInfo;
    memset(&SvcInfo,0,sizeof(SvcInfo));

    if (!mAmlVencParam->GetSVCLimit(SvcInfo) || !SvcInfo.enable) {
        ALOGE("do not support svc function!");
        return;
    }

    addParameter(
            DefineParam(mLayerCount, C2_PARAMKEY_TEMPORAL_LAYERING)
            .withDefault(C2StreamTemporalLayeringTuning::output::AllocShared(0u, 0, 0, 0))
            .withFields({C2F(mLayerCount, m.layerCount).any(),
                         C2F(mLayerCount, m.bLayerCount).any()})
            .withSetter(LayerCountSetter)
            .build());
}




void C2VencComp::IntfImpl::onAvcProfileLevelParam() {
    C2Config::profile_t profile;
    C2Config::level_t   level;

    mAmlVencParam->GetDefaultProfileLevel(profile,level);

    addParameter(
        DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
        .withDefault(new C2StreamProfileLevelInfo::output(
                0u, profile,level))
        .withFields({
            C2F(mProfileLevel, profile).oneOf({
                PROFILE_AVC_BASELINE,
                PROFILE_AVC_MAIN,
            }),
            C2F(mProfileLevel, level).oneOf({
                LEVEL_AVC_1,
                LEVEL_AVC_1B,
                LEVEL_AVC_1_1,
                LEVEL_AVC_1_2,
                LEVEL_AVC_1_3,
                LEVEL_AVC_2,
                LEVEL_AVC_2_1,
                LEVEL_AVC_2_2,
                LEVEL_AVC_3,
                LEVEL_AVC_3_1,
                LEVEL_AVC_3_2,
                LEVEL_AVC_4,
                LEVEL_AVC_4_1,
                LEVEL_AVC_4_2,
                LEVEL_AVC_5,
            }),
        })
        .withSetter(AvcProfileLevelSetter, mSize, mFrameRate, mBitrate)
        .build());
}

void C2VencComp::IntfImpl::onHevcProfileLevelParam() {
    C2Config::profile_t profile;
    C2Config::level_t   level;

    mAmlVencParam->GetDefaultProfileLevel(profile,level);
    addParameter(
        DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
        .withDefault(new C2StreamProfileLevelInfo::output(
                0u, profile, level))
        .withFields({
            C2F(mProfileLevel, profile).oneOf({
                PROFILE_HEVC_MAIN,
            }),
            C2F(mProfileLevel, level).oneOf({
                    LEVEL_HEVC_MAIN_1,       ///< HEVC (H.265) Main Tier Level 1
                    LEVEL_HEVC_MAIN_2,                          ///< HEVC (H.265) Main Tier Level 2
                    LEVEL_HEVC_MAIN_2_1,                        ///< HEVC (H.265) Main Tier Level 2.1
                    LEVEL_HEVC_MAIN_3,                          ///< HEVC (H.265) Main Tier Level 3
                    LEVEL_HEVC_MAIN_3_1,                        ///< HEVC (H.265) Main Tier Level 3.1
                    LEVEL_HEVC_MAIN_4,                          ///< HEVC (H.265) Main Tier Level 4
                    LEVEL_HEVC_MAIN_4_1,                        ///< HEVC (H.265) Main Tier Level 4.1
                    LEVEL_HEVC_MAIN_5,                          ///< HEVC (H.265) Main Tier Level 5
                    LEVEL_HEVC_MAIN_5_1,                        ///< HEVC (H.265) Main Tier Level 5.1
                    LEVEL_HEVC_MAIN_5_2,                        ///< HEVC (H.265) Main Tier Level 5.2
                    LEVEL_HEVC_MAIN_6,                          ///< HEVC (H.265) Main Tier Level 6
                    LEVEL_HEVC_MAIN_6_1,                        ///< HEVC (H.265) Main Tier Level 6.1
                    LEVEL_HEVC_MAIN_6_2,                        ///< HEVC (H.265) Main Tier Level 6.2
            }),
        })
        .withSetter(HevcProfileLevelSetter, mSize, mFrameRate, mBitrate)
        .build());
}

C2R C2VencComp::IntfImpl::SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::input> &oldMe,
    C2P<C2StreamPictureSizeInfo::input> &me) {
    (void)mayBlock;
    C2R res = C2R::Ok();
    ALOGD("SizeSetter oldwidth:%d,oldheight:%d,newwidth:%d,newheight:%d",oldMe.v.width,oldMe.v.height,me.v.width,me.v.height);
    if (me.v.width * me.v.height > mMaxSize.width * mMaxSize.height) {
        res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
        me.set().width = oldMe.v.width;
        ALOGD("SizeSetter check max width & height failed");
    }
    if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
        res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
        me.set().width = oldMe.v.width;
    }
    if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
        res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
        me.set().height = oldMe.v.height;
    }
    return res;
}

C2R C2VencComp::IntfImpl::MaxSizeCalculator(bool mayBlock, C2P<C2StreamMaxBufferSizeInfo::input>& me/*,
                                   const C2P<C2StreamPictureSizeInfo::output>& size*/) /*(bool mayBlock, const C2P<C2StreamMaxBufferSizeInfo::input>& me)*/ {
    (void)mayBlock;
    ALOGD("MaxSizeCalculator enter");

    me.set().value = kLinearBufferSize;
    return C2R::Ok();
}

C2R C2VencComp::IntfImpl::LayerCountSetter(bool mayBlock, C2P<C2StreamTemporalLayeringTuning::output> &me) {
    (void)mayBlock;
    C2R res = C2R::Ok();
    ALOGD("LayerCountSetter enter,layercount:%d,bLayerCount:%d",me.v.m.layerCount,me.v.m.bLayerCount);
    /*if (me.v.period < 1) {
        me.set().mode = C2Config::INTRA_REFRESH_DISABLED;
        me.set().period = 0;
    } else {
        // only support arbitrary mode (cyclic in our case)
        me.set().mode = C2Config::INTRA_REFRESH_ARBITRARY;
    }*/
    return res;
}

/*static C2R InputDelaySetter(
        bool mayBlock,
        C2P<C2PortActualDelayTuning::input> &me,
        const C2P<C2StreamGopTuning::output> &gop) {
    (void)mayBlock;
    uint32_t maxBframes = 0;
    ParseGop(gop.v, nullptr, nullptr, &maxBframes);
    me.set().value = maxBframes;
    return C2R::Ok();
}*/

C2R C2VencComp::IntfImpl::FramerateSetter(bool mayBlock, C2P<C2StreamFrameRateInfo::output> &me) {
    (void)mayBlock;
    C2R res = C2R::Ok();
    me.set().value = me.v.value;
    return res;
}



C2R C2VencComp::IntfImpl::BitrateSetter(bool mayBlock, C2P<C2StreamBitrateInfo::output> &me) {
    (void)mayBlock;
    C2R res = C2R::Ok();
    if (me.v.value <= 4096) {
        me.set().value = 4096;
    }
    return res;
}

C2R C2VencComp::IntfImpl::AvcProfileLevelSetter(
        bool mayBlock,
        C2P<C2StreamProfileLevelInfo::output> &me,
        const C2P<C2StreamPictureSizeInfo::input> &size,
        const C2P<C2StreamFrameRateInfo::output> &frameRate,
        const C2P<C2StreamBitrateInfo::output> &bitrate) {
    (void)mayBlock;
    if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) {
        me.set().profile = PROFILE_AVC_MAIN;
    }

    struct LevelLimits {
        C2Config::level_t level;
        float mbsPerSec;
        uint64_t mbs;
        uint32_t bitrate;
    };
    constexpr LevelLimits kLimits[] = {
        { LEVEL_AVC_1,     1485,    99,     64000 },
        // Decoder does not properly handle level 1b.
         //{ LEVEL_AVC_1B,    1485,   99,   128000 },
        { LEVEL_AVC_1_1,   3000,   396,    192000 },
        { LEVEL_AVC_1_2,   6000,   396,    384000 },
        { LEVEL_AVC_1_3,  11880,   396,    768000 },
        { LEVEL_AVC_2,    11880,   396,   2000000 },
        { LEVEL_AVC_2_1,  19800,   792,   4000000 },
        { LEVEL_AVC_2_2,  20250,  1620,   4000000 },
        { LEVEL_AVC_3,    40500,  1620,  10000000 },
        { LEVEL_AVC_3_1, 108000,  3600,  14000000 },
        { LEVEL_AVC_3_2, 216000,  5120,  20000000 },
        { LEVEL_AVC_4,   245760,  8192,  20000000 },
        { LEVEL_AVC_4_1, 245760,  8192,  50000000 },
        { LEVEL_AVC_4_2, 522240,  8704,  50000000 },
        { LEVEL_AVC_5,   589824, 22080, 135000000 },
    };

    uint64_t mbs = uint64_t((size.v.width + 15) / 16) * ((size.v.height + 15) / 16);
    float mbsPerSec = float(mbs) * frameRate.v.value;

    // Check if the supplied level meets the MB / bitrate requirements. If
    // not, update the level with the lowest level meeting the requirements.

    bool found = false;
    // By default needsUpdate = false in case the supplied level does meet
    // the requirements. For Level 1b, we want to update the level anyway,
    // so we set it to true in that case.
    bool needsUpdate = (me.v.level == LEVEL_AVC_1B);
    for (const LevelLimits &limit : kLimits) {
        if (mbs <= limit.mbs && mbsPerSec <= limit.mbsPerSec &&
                bitrate.v.value <= limit.bitrate) {
            // This is the lowest level that meets the requirements, and if
            // we haven't seen the supplied level yet, that means we don't
            // need the update.
            if (needsUpdate) {
                ALOGD("Given level %x does not cover current configuration: "
                      "adjusting to %x", me.v.level, limit.level);
                me.set().level = limit.level;
            }
            found = true;
            break;
        }
        if (me.v.level == limit.level) {
            // We break out of the loop when the lowest feasible level is
            // found. The fact that we're here means that our level doesn't
            // meet the requirement and needs to be updated.
            needsUpdate = true;
        }
    }
    if (!found) {
        // We set to the highest supported level.
        me.set().level = LEVEL_AVC_5;
    }

    return C2R::Ok();
}

C2R C2VencComp::IntfImpl::HevcProfileLevelSetter(
        bool mayBlock,
        C2P<C2StreamProfileLevelInfo::output> &me,
        const C2P<C2StreamPictureSizeInfo::input> &size,
        const C2P<C2StreamFrameRateInfo::output> &frameRate,
        const C2P<C2StreamBitrateInfo::output> &bitrate) {
    (void)mayBlock;
    if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) {
        me.set().profile = PROFILE_HEVC_MAIN;
    }

    struct LevelLimits {
        C2Config::level_t level;
        uint32_t samplesPerSec;
        uint32_t samples;
        uint32_t bitrate;
    };
    constexpr LevelLimits kLimits[] = {
        { LEVEL_HEVC_MAIN_1,       552960,    36864,    128000 },
        { LEVEL_HEVC_MAIN_2,      3686400,   122880,   1500000 },
        { LEVEL_HEVC_MAIN_2_1,    7372800,   245760,   3000000 },
        { LEVEL_HEVC_MAIN_3,     16588800,   552960,   6000000 },
        { LEVEL_HEVC_MAIN_3_1,   33177600,   983040,  10000000 },
        { LEVEL_HEVC_MAIN_4,     66846720,  2228224,  12000000 },
        { LEVEL_HEVC_MAIN_4_1,  133693440,  2228224,  20000000 },
        { LEVEL_HEVC_MAIN_5,    267386880,  8912896,  25000000 },
        { LEVEL_HEVC_MAIN_5_1,  534773760,  8912896,  40000000 },
        { LEVEL_HEVC_MAIN_5_2, 1069547520,  8912896,  60000000 },
        { LEVEL_HEVC_MAIN_6,   1069547520, 35651584,  60000000 },
        { LEVEL_HEVC_MAIN_6_1, 2139095040, 35651584, 120000000 },
        { LEVEL_HEVC_MAIN_6_2, 4278190080, 35651584, 240000000 },
    };

    uint32_t samples = size.v.width * size.v.height;
    uint32_t samplesPerSec = samples * frameRate.v.value;

    // Check if the supplied level meets the MB / bitrate requirements. If
    // not, update the level with the lowest level meeting the requirements.

    bool found = false;
    // By default needsUpdate = false in case the supplied level does meet
    // the requirements.
    bool needsUpdate = false;
    for (const LevelLimits &limit : kLimits) {
        if (samples <= limit.samples && samplesPerSec <= limit.samplesPerSec &&
                bitrate.v.value <= limit.bitrate) {
            // This is the lowest level that meets the requirements, and if
            // we haven't seen the supplied level yet, that means we don't
            // need the update.
            if (needsUpdate) {
                ALOGD("Given level %x does not cover current configuration: "
                      "adjusting to %x", me.v.level, limit.level);
                me.set().level = limit.level;
            }
            found = true;
            break;
        }
        if (me.v.level == limit.level) {
            // We break out of the loop when the lowest feasible level is
            // found. The fact that we're here means that our level doesn't
            // meet the requirement and needs to be updated.
            needsUpdate = true;
        }
    }
    if (!found) {
        // We set to the highest supported level.
        me.set().level = LEVEL_HEVC_MAIN_5_2;
    }
    return C2R::Ok();
}

C2R C2VencComp::IntfImpl::IntraRefreshSetter(bool mayBlock, C2P<C2StreamIntraRefreshTuning::output> &me) {
    (void)mayBlock;
    C2R res = C2R::Ok();
    if (me.v.period < 1) {
        me.set().mode = C2Config::INTRA_REFRESH_DISABLED;
        me.set().period = 0;
    } else {
        // only support arbitrary mode (cyclic in our case)
        me.set().mode = C2Config::INTRA_REFRESH_ARBITRARY;
    }
    return res;
}

C2R C2VencComp::IntfImpl::GopSetter(bool mayBlock, C2P<C2StreamGopTuning::output> &me) {
    (void)mayBlock;
    for (size_t i = 0; i < me.v.flexCount(); ++i) {
        const C2GopLayerStruct &layer = me.v.m.values[0];
        if (layer.type_ == C2Config::picture_type_t(P_FRAME | B_FRAME)
                && layer.count > HCODEC_MAX_B_FRAMES) {
            me.set().m.values[i].count = HCODEC_MAX_B_FRAMES;
        }
    }
    return C2R::Ok();
}

C2R C2VencComp::IntfImpl::PictureQuantizationSetter(bool mayBlock,
                                     C2P<C2StreamPictureQuantizationTuning::output> &me) {
    (void)mayBlock;
    (void)me;

    // TODO: refactor with same algorithm in the SetQp()
    int32_t iMin = DEFAULT_I_QP_MIN, pMin = DEFAULT_P_QP_MIN, bMin = DEFAULT_B_QP_MIN;
    int32_t iMax = DEFAULT_I_QP_MAX, pMax = DEFAULT_P_QP_MAX, bMax = DEFAULT_B_QP_MAX;

    for (size_t i = 0; i < me.v.flexCount(); ++i) {
        const C2PictureQuantizationStruct &layer = me.v.m.values[i];

        if (layer.type_ == C2Config::picture_type_t(I_FRAME)) {
            iMax = layer.max;
            iMin = layer.min;
            ALOGV("iMin %d iMax %d", iMin, iMax);
        } else if (layer.type_ == C2Config::picture_type_t(P_FRAME)) {
            pMax = layer.max;
            pMin = layer.min;
            ALOGV("pMin %d pMax %d", pMin, pMax);
        } else if (layer.type_ == C2Config::picture_type_t(B_FRAME)) {
            bMax = layer.max;
            bMin = layer.min;
            ALOGV("bMin %d bMax %d", bMin, bMax);
        }
    }

    ALOGV("PictureQuantizationSetter(entry): i %d-%d p %d-%d b %d-%d",
          iMin, iMax, pMin, pMax, bMin, bMax);

    // ensure we have legal values
    iMax = std::clamp(iMax, CODEC_QP_MIN, CODEC_QP_MAX);
    iMin = std::clamp(iMin, CODEC_QP_MIN, CODEC_QP_MAX);
    pMax = std::clamp(pMax, CODEC_QP_MIN, CODEC_QP_MAX);
    pMin = std::clamp(pMin, CODEC_QP_MIN, CODEC_QP_MAX);
    bMax = std::clamp(bMax, CODEC_QP_MIN, CODEC_QP_MAX);
    bMin = std::clamp(bMin, CODEC_QP_MIN, CODEC_QP_MAX);

    // put them back into the structure
    for (size_t i = 0; i < me.v.flexCount(); ++i) {
        const C2PictureQuantizationStruct &layer = me.v.m.values[i];

        if (layer.type_ == C2Config::picture_type_t(I_FRAME)) {
            me.set().m.values[i].max = iMax;
            me.set().m.values[i].min = iMin;
        }
        if (layer.type_ == C2Config::picture_type_t(P_FRAME)) {
            me.set().m.values[i].max = pMax;
            me.set().m.values[i].min = pMin;
        }
        if (layer.type_ == C2Config::picture_type_t(B_FRAME)) {
            me.set().m.values[i].max = bMax;
            me.set().m.values[i].min = bMin;
        }
    }
    ALOGV("PictureQuantizationSetter(exit): i %d-%d p %d-%d b %d-%d",
          iMin, iMax, pMin, pMax, bMin, bMax);

    return C2R::Ok();
}

C2R C2VencComp::IntfImpl::ColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::input> &me) {
    (void)mayBlock;
    if (me.v.range > C2Color::RANGE_OTHER) {
            me.set().range = C2Color::RANGE_OTHER;
    }
    if (me.v.primaries > C2Color::PRIMARIES_OTHER) {
            me.set().primaries = C2Color::PRIMARIES_OTHER;
    }
    if (me.v.transfer > C2Color::TRANSFER_OTHER) {
            me.set().transfer = C2Color::TRANSFER_OTHER;
    }
    if (me.v.matrix > C2Color::MATRIX_OTHER) {
            me.set().matrix = C2Color::MATRIX_OTHER;
    }
    return C2R::Ok();
}

C2R C2VencComp::IntfImpl::CodedColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::output> &me,
                                   const C2P<C2StreamColorAspectsInfo::input> &coded) {
    (void)mayBlock;
    me.set().range = coded.v.range;
    me.set().primaries = coded.v.primaries;
    me.set().transfer = coded.v.transfer;
    me.set().matrix = coded.v.matrix;
    return C2R::Ok();
}

C2R C2VencComp::IntfImpl::StrictValueWithNoDeps(bool mayBlock, C2P<C2StreamPixelFormatInfo::input> &old,
                                      C2P<C2StreamPixelFormatInfo::input> &me) {
    (void)mayBlock;
    if (!me.F(me.v.value).supportsNow(me.v.value)) {
        me.set().value = old.v.value;
    }
    return me.F(me.v.value).validatePossible(me.v.value);
}

bool C2VencComp::IntfImpl::VencParamChangeListener(void *pInst,stParamChangeIndex Index,void *pParam) {
    int *pAvgQp = NULL;
    ePicType *pFrameType = NULL;
    bool bSyncFrameReq = false;
    stPicSize *Size = NULL;
    if (NULL == pInst || NULL== pParam)
        return false;
    C2VencComp::IntfImpl *CurInstance = (C2VencComp::IntfImpl *)pInst;
    ALOGD("Listener update param:%d",Index);
    switch (Index) {
        case E_AVRAGE_QP:
            pAvgQp = (int *)pParam;
            CurInstance->setAverageQp(*pAvgQp);
            break;
        case E_FRAME_TYPE:
            pFrameType = (ePicType *)pParam;
            //CurInstance->setPictureType(*pFrameType);
            break;
        case E_SYNCFRAME_REQUEST:
            bSyncFrameReq = *(bool *)pParam;
            CurInstance->setSyncFrameReq(bSyncFrameReq ? C2_TRUE : C2_FALSE);
            break;
        case E_PIC_SIZE:
            Size = (stPicSize *)pParam;
            CurInstance->SetPicSize(Size->width,Size->height);
            break;
        default:
            break;
    }
    return true;
}



c2_status_t C2VencComp::IntfImpl::config(
            const std::vector<C2Param*> &params, c2_blocking_t mayBlock,
            std::vector<std::unique_ptr<C2SettingResult>>* const failures,
            bool updateParams,
            std::vector<std::shared_ptr<C2Param>> *changes) {
    c2_status_t result;
    result = C2InterfaceHelper::config(params, mayBlock, failures, updateParams, changes);
    ParamUpdate();
    return result;
}



void C2VencComp::IntfImpl::ParamUpdate() {
    stColorAspect ColorAspect;
    stProfileLevelInfo ProfileLevelInfo;
    stVencParam VencParam;

    mAmlVencParam->GetVencParam(VencParam);
    mAmlVencParam->SetSize(mSize->width,mSize->height);
    mAmlVencParam->SetBitrate(mBitrate->value);
    //mIntfImpl->GetVencParam()->SetGopSize(mIntfImpl->getGop()->value);
    mAmlVencParam->SetFrameRate(mFrameRate->value);
    mAmlVencParam->SetPixFormat(mPixelFormat->value);
    mAmlVencParam->SetSyncFrameRequest((C2_TRUE == mRequestSync->value) ? true : false);
    mAmlVencParam->SetCanvasMode(mVencCanvasMode->value);
    mAmlVencParam->SetSyncPeriodSize(mSyncFramePeriod->value);
    mAmlVencParam->SetCanvasMode(mVencCanvasMode->value ? 1 : 0);

    ColorAspect.range = mCodedColorAspects->range;
    ColorAspect.primaries = mCodedColorAspects->primaries;
    ColorAspect.transfer = mCodedColorAspects->transfer;
    ColorAspect.matrix = mCodedColorAspects->matrix;
    mAmlVencParam->SetColorAspect(ColorAspect);

    ProfileLevelInfo.profile = mProfileLevel->profile;
    ProfileLevelInfo.level = mProfileLevel->level;
    mAmlVencParam->SetProfileLevel(ProfileLevelInfo);

    for (size_t i = 0; i < mPictureQuantization->flexCount(); ++i) {
        const C2PictureQuantizationStruct &layer = mPictureQuantization->m.values[i];
        VencParam.QpInfo.enable = true;
        if (layer.type_ == C2Config::picture_type_t(I_FRAME)) {
            VencParam.QpInfo.iMax = layer.max;
            VencParam.QpInfo.iMin = layer.min;
            ALOGV("iMin %d iMax %d", VencParam.QpInfo.iMin, VencParam.QpInfo.iMax);
        } else if (layer.type_ == C2Config::picture_type_t(P_FRAME)) {
            VencParam.QpInfo.pMax = layer.max;
            VencParam.QpInfo.pMin = layer.min;
            ALOGV("pMin %d pMax %d", VencParam.QpInfo.pMin, VencParam.QpInfo.pMax);
        } else if (layer.type_ == C2Config::picture_type_t(B_FRAME)) {
            VencParam.QpInfo.bMax = layer.max;
            VencParam.QpInfo.bMin = layer.min;
            ALOGV("bMin %d bMax %d", VencParam.QpInfo.bMin, VencParam.QpInfo.bMax);
        }
    }
    mAmlVencParam->SetQpInfo(VencParam.QpInfo);
    ALOGD("update Qp Info,min:%d,max:%d!!",VencParam.QpInfo.QpMin,VencParam.QpInfo.QpMax);

    mAmlVencParam->SetLayerCnt(mLayerCount->m.layerCount);
}


}

