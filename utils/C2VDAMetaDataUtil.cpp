#define LOG_NDEBUG 0
#define LOG_TAG "C2VDAMetaDataUtil"

#include <stdio.h>
#include <stdlib.h>
#include <utils/Log.h>
#include <Codec2Mapper.h>
#include <cutils/properties.h>

#include <C2VDAMetaDataUtil.h>
#include <VideoDecodeAcceleratorAdaptor.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <am_gralloc_ext.h>
#include <logdebug.h>
#include <C2VendorConfig.h>

#define V4L2_PARMS_MAGIC 0x55aacc33

#define C2VDAMDU_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, C2VDAComponent::mInstanceID, mComp->mCurInstanceID, ##str)

#define OUTPUT_BUFS_ALIGN_SIZE (64)
#define min(a, b) (((a) > (b))? (b):(a))


namespace android {

constexpr int kMaxWidth8k = 8192;
constexpr int kMaxHeight8k = 4352;
constexpr int kMaxWidth4k = 4096;
constexpr int kMaxHeight4k = 2304;
constexpr int kMaxWidth1080p = 1920;
constexpr int kMaxHeight1080p = 1088;

C2VDAComponent::MetaDataUtil::MetaDataUtil(C2VDAComponent* comp, bool secure):
    mUvmFd(-1),
    mComp(comp),
    mUseSurfaceTexture(false),
    mNoSurface(false),
    mHDRStaticInfoChanged(false),
    mHDR10PLusInfoChanged(false),
    mColorAspectsChanged(false),
    mSecure(secure),
    mEnableNR(false),
    mEnableDILocalBuf(false),
    mIs8k(false),
    mEnable8kNR(false),
    mForceFullUsage(false),
    mIsInterlaced(false),
    mInPtsInvalid(false),
    mFirstOutputWork(false),
    mOutputPtsValid(false),
    mDurationUs(0),
    mCredibleDuration(0),
    mUnstablePts(0),
    mLastOutPts(0),
    mInPutWorkCount(0),
    mOutputWorkCount(0),
    mLastbitStreamId(0),
    mOutputPtsValidCount(0),
    mMarginBufferNum(0),
    mSignalType(0),
    mEnableAdaptivePlayback(false) {
    mIntfImpl = mComp->GetIntfImpl();
    propGetInt(CODEC2_LOGDEBUG_PROPERTY, &gloglevel);
    C2VDAMDU_LOG(CODEC2_LOG_ERR, "[%s:%d]", __func__, __LINE__);

    mUvmFd = amuvm_open();
    if (mUvmFd < 0) {
        C2VDAMDU_LOG(CODEC2_LOG_ERR, "open uvm device fail.");
        mUvmFd = -1;
    }
}

C2VDAComponent::MetaDataUtil::~MetaDataUtil() {
    C2VDAMDU_LOG(CODEC2_LOG_ERR, "[%s:%d]", __func__, __LINE__);
    if (mUvmFd > 0) {
        amuvm_close(mUvmFd);
    }
}

void C2VDAComponent::MetaDataUtil::codecConfig(mediahal_cfg_parms* configParam) {
    uint32_t doubleWriteMode = 3;
    int default_margin = 9;
    uint32_t bufwidth = 4096;
    uint32_t bufheight = 2304;
    uint32_t margin = default_margin;
    bool dvUseTwolaryer = false;
    char value[PROPERTY_VALUE_MAX];

    mEnableNR = property_get_bool("vendor.c2.nr.enable", false);
    mEnableDILocalBuf = property_get_bool("vendor.c2.di.localbuf.enable", false);
    mEnable8kNR = property_get_bool("vendor.c2.8k.nr.enable", false);
    mDisableErrPolicy = property_get_bool("vendor.c2.disable.err.policy", true);

    mConfigParam = configParam;
    memset(mConfigParam, 0, sizeof(mediahal_cfg_parms));
    struct v4l2_parms * pAmlV4l2Param    = &mConfigParam->v4l2cfg;
    struct aml_dec_params * pAmlDecParam = &mConfigParam->amldeccfg;


    std::vector<std::unique_ptr<C2Param>> params;
    C2StreamPictureSizeInfo::output output;
    c2_status_t err = mIntfImpl->query({&output}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VDAMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] query C2StreamPictureSizeInfo size error", __func__, __LINE__);
    }

    C2StreamFrameRateInfo::input inputFrameRateInfo;
    err = mIntfImpl->query({&inputFrameRateInfo}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VDAMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] query C2StreamFrameRateInfo message error", __func__, __LINE__);
    }

    C2StreamUnstablePts::input unstablePts;
    err = mIntfImpl->query({&unstablePts}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VDAMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] query C2StreamUnstablePts message error", __func__, __LINE__);
    } else {
        mUnstablePts = unstablePts.enable;
    }

    if (inputFrameRateInfo.value != 0) {
       mDurationUs = 1000 * 1000 / inputFrameRateInfo.value;
       mCredibleDuration = true;
    } else {
       mDurationUs = 33333; //default 30fps
    }
    C2VDAMDU_LOG(CODEC2_LOG_INFO, "[%s:%d] query frame rate:%f updata mDurationUs = %d, unstablePts :%d",__func__, __LINE__, inputFrameRateInfo.value, mDurationUs, mUnstablePts);

    C2GlobalLowLatencyModeTuning lowLatency;
    err = mIntfImpl->query({&lowLatency}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VDAMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] query C2StreamPictureSizeInfo size error", __func__, __LINE__);
    }

    if (lowLatency.value) {
        C2VDAMDU_LOG(CODEC2_LOG_INFO, "Config low latency mode to v4l2 decoder.");
        pAmlDecParam->cfg.low_latency_mode |= LOWLATENCY_NORMAL;
    } else {
        C2VDAMDU_LOG(CODEC2_LOG_INFO, "disable low latency mode to v4l2 decoder.");
        pAmlDecParam->cfg.low_latency_mode |= LOWLATENCY_DISABALE;
    }

    if (mComp->isDolbyVision()) {
        C2StreamProfileLevelInfo::input inputProfile;
        err = mIntfImpl->query({&inputProfile}, {}, C2_MAY_BLOCK, nullptr);
        if (inputProfile) {
            InputCodec codecType;
            switch (inputProfile.profile)
            {
                case C2Config::PROFILE_DV_AV_PER:
                case C2Config::PROFILE_DV_AV_PEN:
                case C2Config::PROFILE_DV_AV_09:
                    codecType = InputCodec::DVAV;
                    break;
                case C2Config::PROFILE_DV_HE_DER:
                case C2Config::PROFILE_DV_HE_DEN:
                case C2Config::PROFILE_DV_HE_04:
                case C2Config::PROFILE_DV_HE_05:
                case C2Config::PROFILE_DV_HE_DTH:
                case C2Config::PROFILE_DV_HE_07:
                case C2Config::PROFILE_DV_HE_08:
                    codecType = InputCodec::DVHE;
                    break;
                case C2Config::PROFILE_DV_AV1_10:
                    codecType = InputCodec::DVAV1;
                    break;
                default:
                    codecType = InputCodec::UNKNOWN;
                    break;
            }
            C2VDAMDU_LOG(CODEC2_LOG_INFO,"update input Codec profile to %d",codecType);
            if (inputProfile.profile == C2Config::PROFILE_DV_HE_04) {
                dvUseTwolaryer = true;
            }
            mIntfImpl->updateInputCodec(codecType);
        }
    }

    bufwidth = output.width;
    bufheight = output.height;
    C2VDAMDU_LOG(CODEC2_LOG_INFO, "configure width:%d height:%d", output.width, output.height);

    // add v4l2 config
    pAmlV4l2Param->magic = V4L2_PARMS_MAGIC;
    pAmlV4l2Param->len = sizeof(struct v4l2_parms) / sizeof(uint32_t);
    pAmlV4l2Param->adaptiveplayback = mEnableAdaptivePlayback;
    pAmlV4l2Param->width  = bufwidth;
    pAmlV4l2Param->height = bufheight;

    switch (mIntfImpl->getInputCodec())
    {
        case InputCodec::H264:
        case InputCodec::DVAV:
            doubleWriteMode = 0x10;
            break;
        case InputCodec::MP2V:
        case InputCodec::MP4V:
        case InputCodec::MJPG:
            doubleWriteMode = 0x10;
            break;
        case InputCodec::H265:
        case InputCodec::VP9:
        case InputCodec::AV1:
        case InputCodec::DVAV1:
        case InputCodec::DVHE:
            if (mUseSurfaceTexture || mNoSurface) {
                doubleWriteMode = 1;
                C2VDAMDU_LOG(CODEC2_LOG_INFO, "surface texture/nosurface use dw 1");
            } else {
                doubleWriteMode = 3;
            }
            break;
        default:
            doubleWriteMode = 3;
            break;
    }

    if (bufwidth * bufheight > 4096 * 2304) {
        doubleWriteMode = 0x04;
        mIs8k = true;
        if (!mEnable8kNR) {
            mEnableNR = false;
        }
        C2VDAMDU_LOG(CODEC2_LOG_INFO, "[%s:%d] is 8k",__func__, __LINE__);
    }

    if (mIntfImpl->getInputCodec() == InputCodec::H264) {
        doubleWriteMode = 0x10;
    }

    if (bufwidth * bufheight > 1920 * 1088) {
        default_margin = 7;
    }

    if (property_get("vendor.media.doublewrite", value, NULL) > 0) {
        doubleWriteMode = atoi(value);
        C2VDAMDU_LOG(CODEC2_LOG_INFO, "set double:%d", doubleWriteMode);
    }
    memset(value, 0, sizeof(value));
    if (property_get("vendor.media.margin", value, NULL) > 0) {
        default_margin = atoi(value);
        C2VDAMDU_LOG(CODEC2_LOG_INFO, "set margin:%d", default_margin);
    }

    margin = default_margin;
    pAmlDecParam->cfg.canvas_mem_mode = 0;

#if 0
    if (mIntfImpl->getInputCodec() == InputCodec::VP9) {
        doubleWriteMode = 0x03;
        pAmlDecParam->cfg.canvas_mem_mode = 2;
    } else if (mIntfImpl->getInputCodec() == InputCodec::H265) {
        doubleWriteMode = 0x03;
        pAmlDecParam->cfg.canvas_mem_mode = 2;
    } else if (mIntfImpl->getInputCodec() == InputCodec::H264) {
        pAmlDecParam->cfg.canvas_mem_mode = 1;
    }
#endif

    mMarginBufferNum = margin;
    C2VDAMDU_LOG(CODEC2_LOG_INFO, "doubleWriteMode %d, margin:%d \n", doubleWriteMode, margin);
    if (mUseSurfaceTexture || mNoSurface) {
        mEnableNR = false;
        mEnableDILocalBuf = false;
    }

    if (mEnableNR) {
        C2VDAMDU_LOG(CODEC2_LOG_INFO, "enable NR");
        pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_NR_ENABLE;
    }

    if (mEnableDILocalBuf) {
        C2VDAMDU_LOG(CODEC2_LOG_INFO, "enable DILocalBuf");
        pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DI_LOCALBUF_ENABLE;
    }

    if (mDisableErrPolicy) {
        C2VDAMDU_LOG(CODEC2_LOG_INFO, "c2 need disable error policy");
        pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DIS_ERR_POLICY;
    }

    pAmlDecParam->cfg.uvm_hook_type = 2;
    if (/*!mSecureMode*/1) {
        if (mIntfImpl->getInputCodec() == InputCodec::H265) {
            pAmlDecParam->cfg.init_height = bufwidth;
            pAmlDecParam->cfg.init_width = bufheight;
            pAmlDecParam->cfg.ref_buf_margin = margin;
            pAmlDecParam->cfg.double_write_mode = doubleWriteMode;
            pAmlDecParam->cfg.canvas_mem_endian = 0;
            pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DV_NEGATIVE;
            pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
        } else if (mIntfImpl->getInputCodec() == InputCodec::VP9) {
            pAmlDecParam->cfg.init_height = bufwidth;
            pAmlDecParam->cfg.init_width = bufheight;
            pAmlDecParam->cfg.ref_buf_margin = margin;
            pAmlDecParam->cfg.double_write_mode = doubleWriteMode;
            pAmlDecParam->cfg.canvas_mem_endian = 0;
            pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DV_NEGATIVE;
            pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
            setHDRStaticInfo();
        } else if (mIntfImpl->getInputCodec() == InputCodec::H264) {
            pAmlDecParam->cfg.init_height = bufwidth;
            pAmlDecParam->cfg.init_width = bufheight;
            pAmlDecParam->cfg.ref_buf_margin = margin;
            pAmlDecParam->cfg.double_write_mode = doubleWriteMode;
            pAmlDecParam->cfg.canvas_mem_endian = 0;
            pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DV_NEGATIVE;
            pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
        } else if (mIntfImpl->getInputCodec() == InputCodec::AV1) {
            pAmlDecParam->cfg.init_height = bufwidth;
            pAmlDecParam->cfg.init_width = bufheight;
            pAmlDecParam->cfg.ref_buf_margin = margin;
            pAmlDecParam->cfg.double_write_mode = doubleWriteMode;
            pAmlDecParam->cfg.canvas_mem_endian = 0;
            pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DV_NEGATIVE;
            pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
            setHDRStaticInfo();
        } else if (mIntfImpl->getInputCodec() == InputCodec::DVHE) {
            pAmlDecParam->cfg.init_height = bufwidth;
            pAmlDecParam->cfg.init_width = bufheight;
            pAmlDecParam->cfg.ref_buf_margin = margin;
            pAmlDecParam->cfg.double_write_mode = doubleWriteMode;
            pAmlDecParam->cfg.canvas_mem_endian = 0;
            pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
            if (dvUseTwolaryer) {
                pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DV_TWOLARYER;
            }
        } else if (mIntfImpl->getInputCodec() == InputCodec::DVAV) {
            pAmlDecParam->cfg.init_height = bufwidth;
            pAmlDecParam->cfg.init_width = bufheight;
            pAmlDecParam->cfg.ref_buf_margin = margin;
            pAmlDecParam->cfg.double_write_mode = doubleWriteMode;
            pAmlDecParam->cfg.canvas_mem_endian = 0;
            pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
        } else if (mIntfImpl->getInputCodec() == InputCodec::DVAV1) {
            pAmlDecParam->cfg.init_height = bufwidth;
            pAmlDecParam->cfg.init_width = bufheight;
            pAmlDecParam->cfg.ref_buf_margin = margin;
            pAmlDecParam->cfg.double_write_mode = doubleWriteMode;
            pAmlDecParam->cfg.canvas_mem_endian = 0;
            pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
        } else if (mIntfImpl->getInputCodec() == InputCodec::MP2V) {
            pAmlDecParam->cfg.init_height = bufwidth;
            pAmlDecParam->cfg.init_width = bufheight;
            pAmlDecParam->cfg.ref_buf_margin = margin;
            pAmlDecParam->cfg.double_write_mode = doubleWriteMode;
            pAmlDecParam->cfg.canvas_mem_endian = 0;
            pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
        } else if (mIntfImpl->getInputCodec() == InputCodec::MP4V) {
            pAmlDecParam->cfg.init_height = bufwidth;
            pAmlDecParam->cfg.init_width = bufheight;
            pAmlDecParam->cfg.ref_buf_margin = margin;
            pAmlDecParam->cfg.double_write_mode = doubleWriteMode;
            pAmlDecParam->cfg.canvas_mem_endian = 0;
            pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
        }
    }
}

int32_t C2VDAComponent::MetaDataUtil::getUnstablePts() {
    return mUnstablePts;
}
int64_t C2VDAComponent::MetaDataUtil::getLastOutputPts() {
    return mLastOutPts;
}

int C2VDAComponent::MetaDataUtil::setHDRStaticInfo() {
        std::vector<std::unique_ptr<C2Param>> params;
    C2StreamHdrStaticInfo::output hdr;
    bool isPresent = true;
    int32_t matrixCoeffs;
    int32_t transfer;
    int32_t primaries;
    bool    range;

    c2_status_t err = mIntfImpl->query({&hdr}, {}, C2_DONT_BLOCK, &params);
    if (err != C2_OK) {
        C2VDAMDU_LOG(CODEC2_LOG_ERR, "query hdr info error");
        return 0;
    }

    if (((int32_t)(hdr.mastering.red.x * 1000) == 0) &&
        ((int32_t)(hdr.mastering.red.y * 1000) == 0) &&
        ((int32_t)(hdr.mastering.green.x * 1000) == 0) &&
        ((int32_t)(hdr.mastering.green.y * 1000) == 0) &&
        ((int32_t)(hdr.mastering.blue.x * 1000) == 0) &&
        ((int32_t)(hdr.mastering.blue.y * 1000) == 0) &&
        ((int32_t)(hdr.mastering.white.x * 1000) == 0) &&
        ((int32_t)(hdr.mastering.white.y * 1000) == 0) &&
        ((int32_t)(hdr.mastering.maxLuminance * 1000) == 0) &&
        ((int32_t)(hdr.mastering.minLuminance * 1000) == 0) &&
        ((int32_t)(hdr.maxCll * 1000) == 0) &&
        ((int32_t)(hdr.maxFall * 1000) == 0)) { /* defalut val */
        C2VDAMDU_LOG(CODEC2_LOG_INFO, "no hdr static info set");
        return 0;
    }

    bool enable = property_get_bool("vendor.c2.hdr.littleendian.enable", false);

    std::function<int(int)> BLEndianInt = [=] (int value) -> int {
        if (enable)
            return value;
        else
            return ((value & 0x00FF) << 8 ) | ((value & 0xFF00) >> 8);
    };

    struct aml_dec_params *pAmlDecParam = &mConfigParam->amldeccfg;

    pAmlDecParam->hdr.color_parms.present_flag = 1;
    pAmlDecParam->hdr.color_parms.primaries[0][0] = BLEndianInt(hdr.mastering.green.x / 0.00002 + 0.5);//info.sType1.mG.x;
    pAmlDecParam->hdr.color_parms.primaries[0][1] = BLEndianInt(hdr.mastering.green.y / 0.00002 + 0.5);//info.sType1.mG.y;
    pAmlDecParam->hdr.color_parms.primaries[1][0] = BLEndianInt(hdr.mastering.blue.x / 0.00002 + 0.5);//info.sType1.mB.x;
    pAmlDecParam->hdr.color_parms.primaries[1][1] = BLEndianInt(hdr.mastering.blue.y / 0.00002 + 0.5);//info.sType1.mB.y;
    pAmlDecParam->hdr.color_parms.primaries[2][0] = BLEndianInt(hdr.mastering.red.x / 0.00002 + 0.5);//info.sType1.mR.x;
    pAmlDecParam->hdr.color_parms.primaries[2][1] = BLEndianInt(hdr.mastering.red.y / 0.00002 + 0.5);//info.sType1.mR.y;
    pAmlDecParam->hdr.color_parms.white_point[0]  = BLEndianInt(hdr.mastering.white.x / 0.00002 + 0.5);//info.sType1.mW.x;
    pAmlDecParam->hdr.color_parms.white_point[1]  = BLEndianInt(hdr.mastering.white.y / 0.00002 + 0.5);//info.sType1.mW.y;
    pAmlDecParam->hdr.color_parms.luminance[0]    = BLEndianInt(((int32_t)(hdr.mastering.maxLuminance + 0.5))) * 1000;//info.sType1.mMaxDisplayLuminance * 1000;
    pAmlDecParam->hdr.color_parms.luminance[1]    = BLEndianInt(hdr.mastering.minLuminance / 0.0001 + 0.5);//info.sType1.mMinDisplayLuminance;
    pAmlDecParam->hdr.color_parms.content_light_level.max_content     =  BLEndianInt(hdr.maxCll + 0.5);//info.sType1.mMaxContentLightLevel;
    pAmlDecParam->hdr.color_parms.content_light_level.max_pic_average =  BLEndianInt(hdr.maxFall + 0.5);//info.sType1.mMaxFrameAverageLightLevel;
    pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_HDRINFO;

    ColorAspects sfAspects;
    if (!C2Mapper::map(mHDRStaticInfoColorAspects->primaries, &sfAspects.mPrimaries)) {
        sfAspects.mPrimaries = android::ColorAspects::PrimariesUnspecified;
    }
    if (!C2Mapper::map(mHDRStaticInfoColorAspects->range, &sfAspects.mRange)) {
        sfAspects.mRange = android::ColorAspects::RangeUnspecified;
    }
    if (!C2Mapper::map(mHDRStaticInfoColorAspects->matrix, &sfAspects.mMatrixCoeffs)) {
        sfAspects.mMatrixCoeffs = android::ColorAspects::MatrixUnspecified;
    }
    if (!C2Mapper::map(mHDRStaticInfoColorAspects->transfer, &sfAspects.mTransfer)) {
        sfAspects.mTransfer = android::ColorAspects::TransferUnspecified;
    }

    ColorUtils::convertCodecColorAspectsToIsoAspects(sfAspects, &primaries, &transfer, &matrixCoeffs, &range);

    pAmlDecParam->hdr.signal_type = (isPresent << 29)
                                    | (5 << 26)
                                    | (range << 25)
                                    | (1 << 24)
                                    | (primaries << 16)
                                    | (transfer << 8)
                                    | matrixCoeffs;

    C2VDAMDU_LOG(CODEC2_LOG_INFO, "set hdrstaticinfo: gx:%d gy:%d bx:%d by:%d rx:%d,ry:%d wx:%d wy:%d maxlum:%d minlum:%d maxcontent:%d maxpicave:%d signaltype:%x, %f %f %f %f %f %f %f %f %f %f %f %f",
            pAmlDecParam->hdr.color_parms.primaries[0][0],
            pAmlDecParam->hdr.color_parms.primaries[0][1],
            pAmlDecParam->hdr.color_parms.primaries[1][0],
            pAmlDecParam->hdr.color_parms.primaries[1][1],
            pAmlDecParam->hdr.color_parms.primaries[2][0],
            pAmlDecParam->hdr.color_parms.primaries[2][1],
            pAmlDecParam->hdr.color_parms.white_point[0],
            pAmlDecParam->hdr.color_parms.white_point[1],
            pAmlDecParam->hdr.color_parms.luminance[0],
            pAmlDecParam->hdr.color_parms.luminance[1],
            pAmlDecParam->hdr.color_parms.content_light_level.max_content,
            pAmlDecParam->hdr.color_parms.content_light_level.max_pic_average,
            pAmlDecParam->hdr.signal_type,
            hdr.mastering.green.x,
            hdr.mastering.green.y,
            hdr.mastering.blue.x,
            hdr.mastering.blue.y,
            hdr.mastering.red.x,
            hdr.mastering.red.y,
            hdr.mastering.white.x,
            hdr.mastering.white.y,
            hdr.mastering.maxLuminance,
            hdr.mastering.minLuminance,
            hdr.maxCll,
            hdr.maxFall);

    return 0;
}

void C2VDAComponent::MetaDataUtil::updateDecParmInfo(aml_dec_params* pinfo) {
    C2VDAMDU_LOG(CODEC2_LOG_INFO, "pinfo->dec_parms_status %x\n", pinfo->parms_status);
    if (pinfo->parms_status & V4L2_CONFIG_PARM_DECODE_HDRINFO) {
        checkHDRMetadataAndColorAspects(&pinfo->hdr);
    }
}

void C2VDAComponent::MetaDataUtil::updateInterlacedInfo(bool isInterlaced) {
    C2VDAMDU_LOG(CODEC2_LOG_INFO, "%s#%d: isInterlaced:%d", __func__, __LINE__, isInterlaced);
    mIsInterlaced = isInterlaced;
}

void C2VDAComponent::MetaDataUtil::flush() {
    mLastOutPts = 0;
    mOutputPtsValid  = false;
    mFirstOutputWork = false;
    mOutputWorkCount = 0;
    mOutputPtsValidCount = 0;
}

int C2VDAComponent::MetaDataUtil::checkHDRMetadataAndColorAspects(struct aml_vdec_hdr_infos* phdr) {
    bool isHdrChanged = false;
    bool isColorAspectsChanged = false;
    C2StreamHdrStaticInfo::output hdr;

    //setup hdr metadata, only present_flag is 1 there has a hdr metadata
    if (phdr->color_parms.present_flag == 1) {
        hdr.mastering.green.x	= 	phdr->color_parms.primaries[0][0] * 0.00002;
        hdr.mastering.green.y	= 	phdr->color_parms.primaries[0][1] * 0.00002;
        hdr.mastering.blue.x	=  	phdr->color_parms.primaries[1][0] * 0.00002;
        hdr.mastering.blue.y	= 	phdr->color_parms.primaries[1][1] * 0.00002;
        hdr.mastering.red.x     = 	phdr->color_parms.primaries[2][0] * 0.00002;
        hdr.mastering.red.y     = 	phdr->color_parms.primaries[2][1] * 0.00002;
        hdr.mastering.white.x	= 	phdr->color_parms.white_point[0] * 0.00002;
        hdr.mastering.white.y	= 	phdr->color_parms.white_point[1] * 0.00002;

        int32_t MaxDisplayLuminance = 0;
        if (mIntfImpl->getInputCodec() == InputCodec::H265) {
            MaxDisplayLuminance = min(50 * ((phdr->color_parms.luminance[0] + 250000) / 500000), 10000);
            hdr.mastering.maxLuminance = (float)MaxDisplayLuminance;
        } else if (mIntfImpl->getInputCodec() == InputCodec::VP9 || mIntfImpl->getInputCodec() == InputCodec::AV1) {
            MaxDisplayLuminance = phdr->color_parms.luminance[0];
            hdr.mastering.maxLuminance = (float)MaxDisplayLuminance / 1000;
        } else {
            MaxDisplayLuminance = phdr->color_parms.luminance[0];
            hdr.mastering.maxLuminance = (float)MaxDisplayLuminance / 1000;
        }

        hdr.mastering.minLuminance = phdr->color_parms.luminance[1] * 0.0001;
        hdr.maxCll =
            phdr->color_parms.content_light_level.max_content;
        hdr.maxFall =
            phdr->color_parms.content_light_level.max_pic_average;

        isHdrChanged = checkHdrStaticInfoMetaChanged(phdr);
    }

    if (mSignalType != phdr->signal_type) {
        mSignalType = phdr->signal_type;
        //setup color aspects
        bool isPresent       = (phdr->signal_type >> 29) & 0x01;
        int32_t matrixCoeffs = (phdr->signal_type >> 0 ) & 0xff;
        int32_t transfer     = (phdr->signal_type >> 8 ) & 0xff;
        int32_t primaries    = (phdr->signal_type >> 16) & 0xff;
        bool    range        = (phdr->signal_type >> 25) & 0x01;

        if (isPresent) {
            ColorAspects aspects;
            C2StreamColorAspectsInfo::input codedAspects = { 0u };
            ColorUtils::convertIsoColorAspectsToCodecAspects(
                primaries, transfer, matrixCoeffs, range, aspects);
            isColorAspectsChanged = 1;//((OmxVideoDecoder*)mOwner)->handleColorAspectsChange( true, aspects);
            if (!C2Mapper::map(aspects.mPrimaries, &codedAspects.primaries)) {
                codedAspects.primaries = C2Color::PRIMARIES_UNSPECIFIED;
            }
            if (!C2Mapper::map(aspects.mRange, &codedAspects.range)) {
                codedAspects.range = C2Color::RANGE_UNSPECIFIED;
            }
            if (!C2Mapper::map(aspects.mMatrixCoeffs, &codedAspects.matrix)) {
                codedAspects.matrix = C2Color::MATRIX_UNSPECIFIED;
            }
            if (!C2Mapper::map(aspects.mTransfer, &codedAspects.transfer)) {
                codedAspects.transfer = C2Color::TRANSFER_UNSPECIFIED;
            }
            C2VDAMDU_LOG(CODEC2_LOG_INFO, "update color aspect p:%d/%d, r:%d/%d, m:%d/%d, t:%d/%d",
                        codedAspects.primaries, aspects.mPrimaries,
                        codedAspects.range, codedAspects.range,
                        codedAspects.matrix, aspects.mMatrixCoeffs,
                        codedAspects.transfer, aspects.mTransfer);
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            c2_status_t err = mIntfImpl->config({&codedAspects}, C2_MAY_BLOCK, &failures);
            if (err != C2_OK) {
                C2VDAMDU_LOG(CODEC2_LOG_ERR, "Failed to config hdr static info, error:%d", err);
            }
            std::lock_guard<std::mutex> lock(mMutex);
            mColorAspectsChanged = true;

        }
    }



    //notify OMX Client port settings changed if needed
    if (isHdrChanged) {
        //mOwner->eventHandler(OMX_EventPortSettingsChanged, kOutputPortIndex,
        //OMX_IndexAndroidDescribeHDRStaticInfo, NULL)
        //config
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        c2_status_t err = mIntfImpl->config({&hdr}, C2_MAY_BLOCK, &failures);
        if (err != C2_OK) {
            C2VDAMDU_LOG(CODEC2_LOG_ERR, "Failed to config hdr static info, error:%d", err);
        }
        std::lock_guard<std::mutex> lock(mMutex);
        mHDRStaticInfoChanged = true;
    }

    return 0;
}

int C2VDAComponent::MetaDataUtil::checkHdrStaticInfoMetaChanged(struct aml_vdec_hdr_infos* phdr) {
    if ((phdr->color_parms.primaries[0][0] == 0) &&
        (phdr->color_parms.primaries[0][1] == 0) &&
        (phdr->color_parms.primaries[1][0] == 0) &&
        (phdr->color_parms.primaries[1][1] == 0) &&
        (phdr->color_parms.primaries[2][0] == 0) &&
        (phdr->color_parms.primaries[2][1] == 0) &&
        (phdr->color_parms.white_point[0] == 0) &&
        (phdr->color_parms.white_point[1] == 0) &&
        (phdr->color_parms.luminance[0] == 0) &&
        (phdr->color_parms.luminance[1] == 0) &&
        (phdr->color_parms.content_light_level.max_content == 0) &&
        (phdr->color_parms.content_light_level.max_pic_average == 0)) {
        return false;
    }

    if (isHDRStaticInfoDifferent(&(mConfigParam->amldeccfg.hdr), phdr)) {
        mConfigParam->amldeccfg.hdr = *phdr;
        return true;
    }

    return false;
}

int C2VDAComponent::MetaDataUtil::isHDRStaticInfoDifferent(struct aml_vdec_hdr_infos* phdr_old, struct aml_vdec_hdr_infos* phdr_new) {
  if ((phdr_old->color_parms.primaries[0][0] != phdr_new->color_parms.primaries[0][0]) ||
      (phdr_old->color_parms.primaries[0][1] != phdr_new->color_parms.primaries[0][1]) ||
      (phdr_old->color_parms.primaries[1][0] != phdr_new->color_parms.primaries[1][0]) ||
      (phdr_old->color_parms.primaries[1][1] != phdr_new->color_parms.primaries[1][1]) ||
      (phdr_old->color_parms.primaries[2][0] |= phdr_new->color_parms.primaries[2][0]) ||
      (phdr_old->color_parms.primaries[2][1] |= phdr_new->color_parms.primaries[2][1]) ||
      (phdr_old->color_parms.white_point[0] != phdr_new->color_parms.white_point[0]) ||
      (phdr_old->color_parms.white_point[1] != phdr_new->color_parms.white_point[1]) ||
      (phdr_old->color_parms.luminance[0] != phdr_new->color_parms.luminance[0]) ||
      (phdr_old->color_parms.luminance[1] != phdr_new->color_parms.luminance[0]) ||
      (phdr_old->color_parms.content_light_level.max_content != phdr_new->color_parms.content_light_level.max_content) ||
      (phdr_old->color_parms.content_light_level.max_pic_average != phdr_new->color_parms.content_light_level.max_pic_average)) {
      return true;
  }

  return false;
}

int C2VDAComponent::MetaDataUtil::getVideoType() {
    int videotype = AM_VIDEO_4K;//AM_VIDEO_AFBC;

    if (mConfigParam->amldeccfg.cfg.double_write_mode == 0 ||
        mConfigParam->amldeccfg.cfg.double_write_mode == 3) {
        if (mIntfImpl->getInputCodec() == InputCodec::VP9 ||
            mIntfImpl->getInputCodec() == InputCodec::H265 ||
            mIntfImpl->getInputCodec() == InputCodec::AV1) {
            videotype |= AM_VIDEO_AFBC;
        }
    }

    return videotype;
}

int64_t C2VDAComponent::MetaDataUtil::checkAndAdjustOutPts(C2Work* work, int32_t flags) {

    int64_t out_pts = work->worklets.front()->output.ordinal.timestamp.peekull();
    int64_t intput_timestamp = work->input.ordinal.timestamp.peekull();
    int64_t custom_timestamp = work->input.ordinal.customOrdinal.peekull();
    int64_t raw_pts = out_pts;

    uint32_t bitstreamId = static_cast<int32_t>(work->input.ordinal.frameIndex.peeku() & 0x3FFFFFFF);
    uint32_t duration = mDurationUs == 0 ? 33366 : mDurationUs;

    if (!(work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) || mFirstOutputWork) {

        if ((out_pts == 0 && intput_timestamp == 0 && custom_timestamp == 0) ||
            (mUnstablePts && !(flags & PICTURE_FLAG_KEYFRAME))) {
            mOutputPtsValidCount++;
            if (mOutputPtsValidCount >= kOutPutPtsValidNum) {
                mOutputPtsValid = true;
            }

            if ((mOutputPtsValid || mCredibleDuration) && mFirstOutputWork)
                out_pts = mLastOutPts + duration;
        }

        if (!(mInPtsInvalid || mIsInterlaced)) {
            if (bitstreamId == mLastbitStreamId && out_pts == 0 && mFirstOutputWork == true) {
                out_pts = mLastOutPts + duration / 2;
            } else if (bitstreamId != mLastbitStreamId && out_pts == 0 && mFirstOutputWork == true) {
                out_pts = mLastOutPts + duration;
            }
        }

        if (mIsInterlaced && mFirstOutputWork) {
            if (bitstreamId == mLastbitStreamId) {
                out_pts = mLastOutPts + duration / 2;
            } else if (bitstreamId != mLastbitStreamId) {
                int64_t ptsTemp = ( raw_pts >= mLastOutPts) ?  (raw_pts - mLastOutPts) : (mLastOutPts - raw_pts);
                int64_t harfStep = ptsTemp / (duration / 4);

                if (harfStep == 1 || harfStep == 2) {
                    if (raw_pts == 0)
                        out_pts = mLastOutPts + duration / 2;
                } else {
                    out_pts = mLastOutPts + duration / 2;
                }
            }
        }

        //update flag
        if (!mFirstOutputWork && mOutputWorkCount == 0) {
            mFirstOutputWork = true;
        }
        mOutputWorkCount++;
        mLastOutPts = out_pts;
        mLastbitStreamId = bitstreamId;
    }

//for debug
    if (flags & PICTURE_FLAG_KEYFRAME) {
        C2VDAMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1,"checkAndAdjustOutPts: I frame found. %x",flags);
    }
    int64_t render_pts = work->worklets.front()->output.ordinal.timestamp.peekull() + out_pts - work->input.ordinal.timestamp.peekull();
    C2VDAMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1,"checkAndAdjustOutPts(%d):duration:%u(%u),mLastOutPts:%llu,index=%llu, input-pts:%llu output-pts:%llu customOrdinal-pts:%llu(%llu) Final-pts:%llu",
                (!(work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) || mFirstOutputWork),duration,mDurationUs,mLastOutPts,
                work->input.ordinal.frameIndex.peekull(),work->input.ordinal.timestamp.peekull(),
                work->worklets.front()->output.ordinal.timestamp.peekull(),out_pts,custom_timestamp,render_pts);

    return out_pts;
}

uint64_t C2VDAComponent::MetaDataUtil::getPlatformUsage() {
    uint64_t usage = am_gralloc_get_video_decoder_full_buffer_usage();

    if (mUseSurfaceTexture || mNoSurface) {
        usage = am_gralloc_get_video_decoder_OSD_buffer_usage();
    } else {
        char value[PROPERTY_VALUE_MAX];
        if (property_get("vendor.media.doublewrite", value, NULL) > 0) {
            int32_t doublewrite_debug = atoi(value);
            C2VDAMDU_LOG(CODEC2_LOG_INFO, "set double:%d", doublewrite_debug);
            if (doublewrite_debug != 0) {
                switch (doublewrite_debug) {
                    case 1:
                    case 0x10:
                        usage = am_gralloc_get_video_decoder_full_buffer_usage();
                        break;
                    case 2:
                    case 3:
                        usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
                        break;
                    case 4:
                    case 0x100:
                    case 0x200:
                    case 0x300:
                        usage = am_gralloc_get_video_decoder_quarter_buffer_usage();
                        break;
                    default:
                        usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
                        break;
                }
            }
         }else if (mIs8k) {
            usage = am_gralloc_get_video_decoder_quarter_buffer_usage();
            C2VDAMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] is 8k use 1/4 usage:%llx",__func__, __LINE__, usage);
         }else if (mForceFullUsage) {
             usage = am_gralloc_get_video_decoder_full_buffer_usage();
             C2VDAMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] force use full usage:%llx",__func__, __LINE__, usage);
         } else {
            switch (mIntfImpl->getInputCodec())
            {
                case InputCodec::H264:
                case InputCodec::MP2V:
                case InputCodec::MP4V:
                case InputCodec::DVAV:
                case InputCodec::MJPG:
                    usage = am_gralloc_get_video_decoder_full_buffer_usage();
                    break;
                case InputCodec::H265:
                case InputCodec::VP9:
                case InputCodec::AV1:
                case InputCodec::DVHE:
                case InputCodec::DVAV1:
                    usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
                    break;
                default:
                    usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
                    break;
            }
        }
    }

    return usage & C2MemoryUsage::PLATFORM_MASK;
}

void C2VDAComponent::MetaDataUtil::setNoSurface(bool isNoSurface) {
    mNoSurface = isNoSurface;
}

void C2VDAComponent::MetaDataUtil::setForceFullUsage(bool isFullUsage) {
    mForceFullUsage = isFullUsage;
}


uint32_t C2VDAComponent::MetaDataUtil::getOutAlignedSize(uint32_t size, bool forcealign) {
    if ((mSecure && mIntfImpl->getInputCodec() == InputCodec::H264) || forcealign)
        return (size + OUTPUT_BUFS_ALIGN_SIZE - 1) & (~(OUTPUT_BUFS_ALIGN_SIZE - 1));
    return size;
}

bool C2VDAComponent::MetaDataUtil::getNeedReallocBuffer()
{
    bool realloc = true;
    bool debugrealloc = property_get_bool("vendor.media.codec2.reallocbuf", false);

    if (debugrealloc)
        return true;

    if (mUseSurfaceTexture|| mNoSurface) {
        realloc = true;
    } else {
        switch (mIntfImpl->getInputCodec()) {
            case InputCodec::H264:
            case InputCodec::MP2V:
            case InputCodec::MP4V:
            case InputCodec::MJPG:
                realloc = true;
                break;
            case InputCodec::H265:
            case InputCodec::VP9:
            case InputCodec::AV1:
                realloc = false;
                break;
            default:
                break;
        }
    }
    return realloc;
}

bool C2VDAComponent::MetaDataUtil::checkReallocOutputBuffer(VideoFormat video_format_old,VideoFormat old_video_format_new)
{
    bool buffernumchanged = false;
    bool framesizechanged = false;

    if (old_video_format_new.mMinNumBuffers != video_format_old.mMinNumBuffers)
        buffernumchanged = true;

    if (old_video_format_new.mCodedSize.width() != video_format_old.mCodedSize.width() ||
        old_video_format_new.mCodedSize.height() !=  video_format_old.mCodedSize.height())
        framesizechanged = true;

    if (buffernumchanged || framesizechanged) {
        return true;
    }
    return false;
}

bool C2VDAComponent::MetaDataUtil::getMaxBufWidthAndHeight(uint32_t* width, uint32_t* height) {
    bool support_4k = property_get_bool("ro.vendor.platform.support.4k", true);

    if (support_4k) {
        if (mIs8k) {
            *width = kMaxWidth8k;
            *height = kMaxHeight8k;
        } else {
            *width = kMaxWidth4k;
            *height = kMaxHeight4k;
        }
    } else {
        *width = kMaxWidth1080p;
        *height = kMaxHeight1080p;
    }
    return true;
}

bool C2VDAComponent::MetaDataUtil::getUvmMetaData(int fd, unsigned char *data, int *size) {
    if (mUvmFd <= 0) {
        mUvmFd = amuvm_open();
        if (mUvmFd < 0) {
            C2VDAMDU_LOG(CODEC2_LOG_ERR, "open uvm device fail.");
            return false;
        }
    }

    if (data == NULL)
        return false;

    int meta_size = amuvm_getmetadata(mUvmFd, fd, data);
    if (meta_size < 0) {
        return false;
    }

    *size = meta_size;
    return true;
}

void C2VDAComponent::MetaDataUtil::parseAndprocessMetaData(unsigned char *data, int size) {
    struct aml_meta_head_s *meta_head;
    uint32_t offset = 0;
    uint32_t meta_magic = 0, meta_type = 0, meta_size = 0;

    if (data == NULL || size <= 0) {
        C2VDAMDU_LOG(CODEC2_LOG_ERR,"parse and process meta data faild");
        return;
    }
    meta_head = (struct aml_meta_head_s *)data;
    while ((offset + AML_META_HEAD_SIZE) < size) {
        meta_magic = meta_head->magic;
        meta_type  = meta_head->type;
        meta_size  = meta_head->data_size;
        if (meta_magic != META_DATA_MAGIC ||
            (meta_size > META_DATA_SIZE) ||
            (meta_size <= 0)) {
            C2VDAMDU_LOG(CODEC2_LOG_ERR,"get mate head error");
            break;
        }
        unsigned char buf[meta_size];
        memset(buf, 0, meta_size);
        if ((offset + AML_META_HEAD_SIZE + meta_size) > size) {
            C2VDAMDU_LOG(CODEC2_LOG_ERR,"metadata oversize %d > %d, please check",
                    offset + AML_META_HEAD_SIZE + meta_size, size);
            break;
        }

        memcpy(buf, (data + offset + AML_META_HEAD_SIZE), meta_size);
        offset = offset + AML_META_HEAD_SIZE + meta_size;
        meta_head = (struct aml_meta_head_s *)(&data[offset]);

        if (meta_type == UVM_META_DATA_VF_BASE_INFOS) {
            updateDurationUs(buf, meta_size);
        } else if (meta_type == UVM_META_DATA_HDR10P_DATA) {
            updateHDR10plus(buf, meta_size);
        }
    }
}

void C2VDAComponent::MetaDataUtil::updateHDR10plus(unsigned char *data, int size) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (size > 0) {
        mHDR10PLusInfoChanged = true;
        char buffer[size];
        memset(buffer,0,size);
        memcpy(buffer, data, size);
        mHDR10PlusData.push({std::string(buffer)});
    }
}
bool C2VDAComponent::MetaDataUtil::getHDR10PlusData(std::string &data)
{
    if (!mHDR10PlusData.empty()) {
        data = std::move(mHDR10PlusData.front());
        mHDR10PlusData.pop();
        return true;
    }
    return false;
}

void C2VDAComponent::MetaDataUtil::save_stream_info(uint64_t timestamp, int filledlen) {
    if (mInPutWorkCount == 0) {
        mAmlStreamInfo.pts_0 = timestamp;
        mAmlStreamInfo.len_0 = filledlen;
    } else if (mInPutWorkCount == 1) {
        mAmlStreamInfo.pts_1 = timestamp;
        mAmlStreamInfo.len_1 = filledlen;
    } else if (mInPutWorkCount == 2) {
        mAmlStreamInfo.pts_2 = timestamp;
        mAmlStreamInfo.len_2 = filledlen;
    } else if (mInPutWorkCount == 3) {
        check_stream_info();
    }
    mInPutWorkCount++;
}

void C2VDAComponent::MetaDataUtil::check_stream_info() {
    C2VDAMDU_LOG(CODEC2_LOG_INFO,"check_stream_info mInPutWorkCount %llu\n", mInPutWorkCount);
    if (mInPutWorkCount == 3 && mAmlStreamInfo.pts_0 == 0
            && mAmlStreamInfo.pts_1 == 0
            && mAmlStreamInfo.pts_2 == 0) {
        C2VDAMDU_LOG(CODEC2_LOG_INFO,"first 3 pts all 0, pts invaild, use default framerate");
        mInPtsInvalid = true;
        return;
    }
}

void C2VDAComponent::MetaDataUtil::updateDurationUs(unsigned char *data, int size) {
    uint32_t durationdata = 0;
    if (data == NULL || size <= 0) {
        C2VDAMDU_LOG(CODEC2_LOG_ERR,"update DurationUs error");
        return;
    }

    struct aml_vf_base_info_s *baseinfo = (struct aml_vf_base_info_s *)(data);

    if (baseinfo != NULL && baseinfo->duration != 0) {
        durationdata = baseinfo->duration;
        if (durationdata != 0) {
            uint64_t rate64 = 1000000;
            rate64 = rate64 / (96000 * 1.0 / durationdata);
            if (mIsInterlaced)
                mDurationUs = 2 * rate64;
            else
                mDurationUs = rate64;
            mCredibleDuration = true;
            C2VDAMDU_LOG(CODEC2_LOG_ERR,"update mDurationUs = %d by meta data", mDurationUs);
        }
    }
}
}
