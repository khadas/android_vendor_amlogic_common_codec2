#define LOG_NDEBUG 0
#define LOG_TAG "C2VdecDeviceUtil"

#include <stdio.h>
#include <stdlib.h>
#include <utils/Log.h>
#include <Codec2Mapper.h>
#include <cutils/properties.h>

#include <C2VdecDeviceUtil.h>
#include <C2VdecInterfaceImpl.h>
#include <VideoDecodeAcceleratorAdaptor.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <am_gralloc_ext.h>
#include <C2VendorProperty.h>
#include <c2logdebug.h>
#include <C2VendorConfig.h>
#include <inttypes.h>

#define V4L2_PARMS_MAGIC 0x55aacc33

#define C2VdecMDU_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d#%d##%d]"#fmt, mPlayerId, mComp->mCurInstanceID, C2VdecComponent::mInstanceNum, ##str)

#define OUTPUT_BUFS_ALIGN_SIZE (64)
#define min(a, b) (((a) > (b))? (b):(a))


namespace android {

constexpr int kMaxWidth8k = 8192;
constexpr int kMaxHeight8k = 4352;
constexpr int kMaxWidth4k = 4096;
constexpr int kMaxHeight4k = 2304;
constexpr int kMaxWidth1080p = 1920;
constexpr int kMaxHeight1080p = 1088;
constexpr int kMaxWidthP010 = 720;
constexpr int kMaxHeightP010 = 576;

C2VdecComponent::DeviceUtil::DeviceUtil(C2VdecComponent* comp, bool secure):
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
    mEnableAvc4kMMU(false),
    mDurationUs(0),
    mDurationUsFromApp(0),
    mCredibleDuration(0),
    mUnstablePts(0),
    mPlayerId(0),
    mLastOutPts(0),
    mInPutWorkCount(0),
    mOutputWorkCount(0),
    mOutputPtsValidCount(0),
    mMarginBufferNum(0),
    mStreamBitDepth(-1),
    mBufferWidth(0),
    mBufferHeight(0),
    mSignalType(0),
    mEnableAdaptivePlayback(false) {
    mVideoDecWraper = NULL;
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
    mIntfImpl = mComp->GetIntfImpl();
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);

    mUvmFd = amuvm_open();
    if (mUvmFd < 0) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "Open uvm device fail.");
        mUvmFd = -1;
    }
}

C2VdecComponent::DeviceUtil::~DeviceUtil() {
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
    if (mUvmFd > 0) {
        amuvm_close(mUvmFd);
    }
}


uint32_t C2VdecComponent::DeviceUtil::getDoubleWriteModeValue() {
    uint32_t doubleWriteValue = 3;
    InputCodec codec = mIntfImpl->getInputCodec();

    int32_t defaultDoubleWrite = getPropertyDoubleWrite();
    if (defaultDoubleWrite >= 0) {
        doubleWriteValue = defaultDoubleWrite;
        CODEC2_LOG(CODEC2_LOG_INFO, "set double write(%d) from property", doubleWriteValue);
        return doubleWriteValue;
    }

    switch (codec) {
        case InputCodec::H264:
        case InputCodec::DVAV:
            doubleWriteValue = 0x10;
            break;
        case InputCodec::MP2V:
        case InputCodec::MP4V:
        case InputCodec::MJPG:
            doubleWriteValue = 0x10;
            break;
        case InputCodec::H265:
        case InputCodec::VP9:
        case InputCodec::AV1:
        case InputCodec::DVAV1:
        case InputCodec::DVHE:
            if (mComp->isNonTunnelMode() & (mUseSurfaceTexture || mNoSurface)) {
                doubleWriteValue = 1;
                if (isYcbcRP010Stream()) {
                    doubleWriteValue = 3;
                }
                CODEC2_LOG(CODEC2_LOG_INFO, "surface texture/nosurface use dw 1");
            } else if (codec == InputCodec::H265 && mIsInterlaced) {
                doubleWriteValue = 0x10;
            } else {
                doubleWriteValue = 3;
            }
            break;
        case InputCodec::AVS2:
            doubleWriteValue = 1;
            break;
        case InputCodec::AVS:
            doubleWriteValue = 1;
            break;
        default:
            doubleWriteValue = 3;
            break;
    }

    if (codec == InputCodec::H264) {
        doubleWriteValue = 0x10;
    }

    if (shouldEnableMMU()) {
        doubleWriteValue = 3;
        CODEC2_LOG(CODEC2_LOG_INFO, "H264 4k mmu :DoubleWrite %d", doubleWriteValue);
        return doubleWriteValue;
    }

    CODEC2_LOG(CODEC2_LOG_INFO, "component double write value:%d", doubleWriteValue);
    return doubleWriteValue;
}

void C2VdecComponent::DeviceUtil::queryStreamBitDepth() {
    int32_t bitdepth = -1;
    VideoDecWraper *videoWraper = mComp->getCompVideoDecWraper();
    AmlMessageBase *msg = VideoDecWraper::AmVideoDec_getAmlMessage();

    if (msg != NULL && videoWraper != NULL) {
        msg->setInt32("bitdepth", bitdepth);
        videoWraper->postAndReplyMsg(msg);
        msg->findInt32("bitdepth", &bitdepth);
        if (bitdepth == 0 || bitdepth == 8 || bitdepth == 10) {
            mStreamBitDepth = bitdepth;
            C2VdecMDU_LOG(CODEC2_LOG_INFO, "Query the stream bit depth(%d) success.", bitdepth);
        } else {
            C2VdecMDU_LOG(CODEC2_LOG_ERR, "Query the stream bit depth failed.");
        }
    }

    if (msg != NULL)
        delete msg;
}

uint32_t C2VdecComponent::DeviceUtil::getStreamPixelFormat(uint32_t pixelFormat) {
    uint32_t format = pixelFormat;
    if (isYcbcRP010Stream()) {
        format = HAL_PIXEL_FORMAT_YCBCR_P010;
    }
    return format;
}

int C2VdecComponent::DeviceUtil::setVideoDecWraper(VideoDecWraper* videoDecWraper) {
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "setVideoDecWraper into.[%p]", videoDecWraper);
    if (videoDecWraper)
        mVideoDecWraper = videoDecWraper;
    return 0;
}

void C2VdecComponent::DeviceUtil::codecConfig(mediahal_cfg_parms* configParam) {
    uint32_t doubleWriteMode = 3;
    int default_margin = 6;
    uint32_t bufwidth = 4096;
    uint32_t bufheight = 2304;
    uint32_t margin = default_margin;
    bool dvUseTwoLayer = false;
    char value[PROPERTY_VALUE_MAX];

    mEnableNR = property_get_bool(C2_PROPERTY_VDEC_DISP_NR_ENABLE, false);
    mEnableDILocalBuf = property_get_bool(C2_PROPERTY_VDEC_DISP_DI_LOCALBUF_ENABLE, false);
    mEnable8kNR = property_get_bool(C2_PROPERTY_VDEC_DISP_NR_8K_ENABLE, false);
    mDisableErrPolicy = property_get_bool(C2_PROPERTY_VDEC_ERRPOLICY_DISABLE, true);

    mConfigParam = configParam;
    memset(mConfigParam, 0, sizeof(mediahal_cfg_parms));
    struct v4l2_parms * pAmlV4l2Param    = &mConfigParam->v4l2_cfg;
    struct aml_dec_params * pAmlDecParam = &mConfigParam->aml_dec_cfg;


    std::vector<std::unique_ptr<C2Param>> params;
    C2StreamPictureSizeInfo::output output;
    c2_status_t err = mIntfImpl->query({&output}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] Query C2StreamPictureSizeInfo size error", __func__, __LINE__);
    }

    C2StreamFrameRateInfo::input inputFrameRateInfo;
    err = mIntfImpl->query({&inputFrameRateInfo}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] Query C2StreamFrameRateInfo message error", __func__, __LINE__);
    }

    C2StreamUnstablePts::input unstablePts;
    err = mIntfImpl->query({&unstablePts}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] Query C2StreamUnstablePts message error", __func__, __LINE__);
    } else {
        mUnstablePts = unstablePts.enable;
    }

    C2VendorPlayerId::input playerId;
    err = mIntfImpl->query({&playerId}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] Query C2VendorPlayerId message error", __func__, __LINE__);
    } else {
        mPlayerId = playerId.value;
    }

    if (inputFrameRateInfo.value != 0) {
       mDurationUs = 1000 * 1000 / inputFrameRateInfo.value;
       mCredibleDuration = true;
    }

    mDurationUsFromApp = mDurationUs;
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "[%s:%d] query frame rate:%f updata mDurationUs = %d, unstablePts :%d",__func__, __LINE__, inputFrameRateInfo.value, mDurationUs, mUnstablePts);
    C2GlobalLowLatencyModeTuning lowLatency;
    err = mIntfImpl->query({&lowLatency}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] query C2StreamPictureSizeInfo size error", __func__, __LINE__);
    }

    if (lowLatency.value) {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "Config low latency mode to v4l2 decoder.");
        pAmlDecParam->cfg.low_latency_mode |= LOWLATENCY_NORMAL;
    } else {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "Disable low latency mode to v4l2 decoder.");
        pAmlDecParam->cfg.low_latency_mode |= LOWLATENCY_DISABLE;
    }

    if (mIntfImpl->mAvc4kMMUMode->value ||
           property_get_bool(C2_PROPERTY_VDEC_ENABLE_AVC_4K_MMU, false)) {
        mEnableAvc4kMMU = true;
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "mEnableAvc4kMMU = %d", mEnableAvc4kMMU);
    } else {
         mEnableAvc4kMMU = false;
    }

    if (mComp->isAmDolbyVision()) {
        dvUseTwoLayer = checkDvProfileAndLayer();
    }

    bufwidth = output.width;
    bufheight = output.height;
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "configure width:%d height:%d", output.width, output.height);

    // add v4l2 config
    pAmlV4l2Param->magic = V4L2_PARMS_MAGIC;
    pAmlV4l2Param->len = sizeof(struct v4l2_parms) / sizeof(uint32_t);
    pAmlV4l2Param->adaptivePlayback = mEnableAdaptivePlayback;
    pAmlV4l2Param->width  = bufwidth;
    pAmlV4l2Param->height = bufheight;

    mBufferWidth  = bufwidth;
    mBufferHeight = bufheight;
    if (bufwidth * bufheight > 4096 * 2304) {
        doubleWriteMode = 0x04;
        default_margin = 5;
        mIs8k = true;
        if (!mEnable8kNR) {
            mEnableNR = false;
        }
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "[%s:%d] is 8k",__func__, __LINE__);
    }

    if (mIntfImpl->getInputCodec() == InputCodec::H264) {
        doubleWriteMode = 0x10;
    }

    doubleWriteMode = getDoubleWriteModeValue();
    memset(value, 0, sizeof(value));
    if (property_get(C2_PROPERTY_VDEC_MARGIN, value, NULL) > 0) {
        default_margin = atoi(value);
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "set margin:%d", default_margin);
    }
    margin = default_margin;

    pAmlDecParam->cfg.canvas_mem_mode = 0;
    mMarginBufferNum = margin;
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "DoubleWriteMode %d, margin:%d \n", doubleWriteMode, margin);

    if (mUseSurfaceTexture || mNoSurface) {
        mEnableNR = false;
        mEnableDILocalBuf = false;
    }

    if (mEnableNR) {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "Enable NR");
        pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_NR_ENABLE;
    }

    if (mEnableDILocalBuf) {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "Enable DILocalBuf");
        pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DI_LOCALBUF_ENABLE;
    }

    if (mDisableErrPolicy) {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "C2 need disable error policy");
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
            if (dvUseTwoLayer) {
                pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DV_TWOLAYER;
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
        } else if (mIntfImpl->getInputCodec() == InputCodec::MJPG) {
            pAmlDecParam->cfg.init_height = bufwidth;
            pAmlDecParam->cfg.init_width = bufheight;
            pAmlDecParam->cfg.ref_buf_margin = margin;
            pAmlDecParam->cfg.double_write_mode = doubleWriteMode;
            pAmlDecParam->cfg.canvas_mem_endian = 0;
            pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
        } else if (mIntfImpl->getInputCodec() == InputCodec::AVS2) {
            pAmlDecParam->cfg.init_height = bufwidth;
            pAmlDecParam->cfg.init_width = bufheight;
            pAmlDecParam->cfg.ref_buf_margin = margin;
            pAmlDecParam->cfg.double_write_mode = doubleWriteMode;
            pAmlDecParam->cfg.canvas_mem_endian = 0;
            pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
        } else if (mIntfImpl->getInputCodec() == InputCodec::AVS) {
            pAmlDecParam->cfg.init_height = bufwidth;
            pAmlDecParam->cfg.init_width = bufheight;
            pAmlDecParam->cfg.ref_buf_margin = margin;
            pAmlDecParam->cfg.double_write_mode = doubleWriteMode;
            pAmlDecParam->cfg.canvas_mem_endian = 0;
            pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;
        }
    }
}


bool C2VdecComponent::DeviceUtil::setUnstable()
{
    bool ret = false;
    C2VdecMDU_LOG(CODEC2_LOG_INFO,"into set mUnstablePts = %d ", mUnstablePts);
    AmlMessageBase *msg = VideoDecWraper::AmVideoDec_getAmlMessage();
    if (msg != NULL) {
        msg->setInt32("unstable", mUnstablePts);
        mVideoDecWraper->postAndReplyMsg(msg);
        ret = true;
    }
    if (msg != NULL)
        delete msg;
    return ret;
}

bool C2VdecComponent::DeviceUtil::setDuration()
{
    bool ret = false;
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "into set mDurationUs = %d ", mDurationUs);
    AmlMessageBase *msg = VideoDecWraper::AmVideoDec_getAmlMessage();
    if (msg != NULL && mDurationUs != 0) {
        msg->setInt32("duration", mDurationUs);
        msg->setInt32("type", 2);
        mVideoDecWraper->postAndReplyMsg(msg);
        ret = true;
    }
    if (msg != NULL) {
        delete msg;
    }
    return ret;
}

void C2VdecComponent::DeviceUtil::setLastOutputPts(uint64_t pts) {
    mLastOutPts = pts;
}

uint64_t C2VdecComponent::DeviceUtil::getLastOutputPts() {
    return mLastOutPts;
}

int C2VdecComponent::DeviceUtil::setHDRStaticInfo() {
        std::vector<std::unique_ptr<C2Param>> params;
    C2StreamHdrStaticInfo::output hdr;
    bool isPresent = true;
    int32_t matrixCoeffs;
    int32_t transfer;
    int32_t primaries;
    bool    range;

    c2_status_t err = mIntfImpl->query({&hdr}, {}, C2_DONT_BLOCK, &params);
    if (err != C2_OK) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "Query hdr info error");
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
        ((int32_t)(hdr.maxFall * 1000) == 0)) { /* default val */
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "No hdr static info set");
        return 0;
    }

    bool enable = property_get_bool(C2_PROPERTY_VDEC_HDR_LITTLE_ENDIAN_ENABLE, false);

    std::function<int(int)> BLEndianInt = [=] (int value) -> int {
        if (enable)
            return value;
        else
            return ((value & 0x00FF) << 8 ) | ((value & 0xFF00) >> 8);
    };

    struct aml_dec_params *pAmlDecParam = &mConfigParam->aml_dec_cfg;

    pAmlDecParam->hdr.color_parms.present_flag = 1;

    if ((mIntfImpl->getInputCodec() == InputCodec::VP9)) {
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
    } else if ((mIntfImpl->getInputCodec() == InputCodec::AV1)) {
        //see 6.7.4. Metadata high dynamic range mastering display color volume
        //semantics
        //hdr_mdcv.primaries* values are in 0.16 fixed-point format.
        //so we will shift left 16bit change to int farmat value.
        //The increase of 0.5 is to ensure that data will not be lost downward
        pAmlDecParam->hdr.color_parms.primaries[0][0] = (int32_t)(hdr.mastering.red.x * 65536.0 + 0.5);//info.sType1.mR.x;
        pAmlDecParam->hdr.color_parms.primaries[0][1] = (int32_t)(hdr.mastering.red.y * 65536.0 + 0.5);//info.sType1.mR.y;
        pAmlDecParam->hdr.color_parms.primaries[1][0] = (int32_t)(hdr.mastering.green.x * 65536.0 + 0.5);//info.sType1.mG.x;
        pAmlDecParam->hdr.color_parms.primaries[1][1] = (int32_t)(hdr.mastering.green.y * 65536.0 + 0.5);//info.sType1.mG.y;
        pAmlDecParam->hdr.color_parms.primaries[2][0] = (int32_t)(hdr.mastering.blue.x * 65536.0 + 0.5);//info.sType1.mB.x;
        pAmlDecParam->hdr.color_parms.primaries[2][1] = (int32_t)(hdr.mastering.blue.y * 65536.0 + 0.5);//info.sType1.mB.y;
        pAmlDecParam->hdr.color_parms.white_point[0]  = (int32_t)(hdr.mastering.white.x * 65536.0 + 0.5);//info.sType1.mW.x;
        pAmlDecParam->hdr.color_parms.white_point[1]  = (int32_t)(hdr.mastering.white.y * 65536.0 + 0.5);//info.sType1.mW.y;
        // hdr_mdcv.luminance_max is in 24.8 fixed-point format.
        //so we will shift left 8bit change to int farmat value.
        //The increase of 0.5 is to ensure that data will not be lost downward
        pAmlDecParam->hdr.color_parms.luminance[0]    = (((int32_t)(hdr.mastering.maxLuminance * 256.0 + 0.5)));//info.sType1.mMaxDisplayLuminance * 1000;
        // hdr_mdcv.luminance_min is in 18.14 format.
        //so we will shift left 14bit change to int farmat value.
        //The increase of 0.5 is to ensure that data will not be lost downward
        pAmlDecParam->hdr.color_parms.luminance[1]    = (int32_t)(hdr.mastering.minLuminance * 16384.0 + 0.5);//info.sType1.mMinDisplayLuminance;
        pAmlDecParam->hdr.color_parms.content_light_level.max_content     =  (int32_t)(hdr.maxCll);//info.sType1.mMaxContentLightLevel;
        pAmlDecParam->hdr.color_parms.content_light_level.max_pic_average =  (int32_t)(hdr.maxFall);//info.sType1.mMaxFrameAverageLightLevel;
    } else if ((mIntfImpl->getInputCodec() == InputCodec::H265)) {
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
    } else {
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
    }


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

    C2VdecMDU_LOG(CODEC2_LOG_INFO, "Set hdrstaticinfo: gx:%d gy:%d bx:%d by:%d rx:%d,ry:%d wx:%d wy:%d maxlum:%d minlum:%d maxcontent:%d maxpicave:%d signaltype:%x, %f %f %f %f %f %f %f %f %f %f %f %f",
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

void C2VdecComponent::DeviceUtil::updateDecParmInfo(aml_dec_params* pInfo) {
    C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Parms status %x\n", pInfo->parms_status);
    if (pInfo->parms_status & V4L2_CONFIG_PARM_DECODE_HDRINFO) {
        checkHDRMetadataAndColorAspects(&pInfo->hdr);
    }
}

void C2VdecComponent::DeviceUtil::updateInterlacedInfo(bool isInterlaced) {
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "[%s:%d] isInterlaced:%d", __func__, __LINE__, isInterlaced);
    mIsInterlaced = isInterlaced;
}

void C2VdecComponent::DeviceUtil::flush() {
    mLastOutPts = 0;
    mOutputPtsValid  = false;
    mFirstOutputWork = false;
    mOutputWorkCount = 0;
    mOutputPtsValidCount = 0;
}

int C2VdecComponent::DeviceUtil::checkHDRMetadataAndColorAspects(struct aml_vdec_hdr_infos* phdr) {
    bool isHdrChanged = false;
    bool isColorAspectsChanged = false;
    C2StreamHdrStaticInfo::output hdr;

    //setup hdr metadata, only present_flag is 1 there has a hdr metadata
    if (phdr->color_parms.present_flag == 1) {
        if ((mIntfImpl->getInputCodec() == InputCodec::VP9)) {
            hdr.mastering.green.x	= 	phdr->color_parms.primaries[0][0] * 0.00002;
            hdr.mastering.green.y	= 	phdr->color_parms.primaries[0][1] * 0.00002;
            hdr.mastering.blue.x	=  	phdr->color_parms.primaries[1][0] * 0.00002;
            hdr.mastering.blue.y	= 	phdr->color_parms.primaries[1][1] * 0.00002;
            hdr.mastering.red.x     = 	phdr->color_parms.primaries[2][0] * 0.00002;
            hdr.mastering.red.y     = 	phdr->color_parms.primaries[2][1] * 0.00002;
            hdr.mastering.white.x	= 	phdr->color_parms.white_point[0] * 0.00002;
            hdr.mastering.white.y	= 	phdr->color_parms.white_point[1] * 0.00002;

            int32_t MaxDisplayLuminance = 0;
            MaxDisplayLuminance = phdr->color_parms.luminance[0];
            hdr.mastering.maxLuminance = (float)MaxDisplayLuminance / 1000;

            hdr.mastering.minLuminance = phdr->color_parms.luminance[1] * 0.0001;
            hdr.maxCll =
                phdr->color_parms.content_light_level.max_content;
            hdr.maxFall =
                phdr->color_parms.content_light_level.max_pic_average;
        } else if ((mIntfImpl->getInputCodec() == InputCodec::AV1)) {
            //see 6.7.4. Metadata high dynamic range mastering display color volume
            //semantics
            //hdr_mdcv.primaries* values are in 0.16 fixed-point format.
            //so we will shift right 16bit change to float 0.16 farmat value.
            hdr.mastering.red.x     = 	phdr->color_parms.primaries[0][0] / 65536.0;
            hdr.mastering.red.y     = 	phdr->color_parms.primaries[0][1] / 65536.0;
            hdr.mastering.green.x	= 	phdr->color_parms.primaries[1][0] / 65536.0;
            hdr.mastering.green.y	= 	phdr->color_parms.primaries[1][1] / 65536.0;
            hdr.mastering.blue.x	=  	phdr->color_parms.primaries[2][0] / 65536.0;
            hdr.mastering.blue.y	= 	phdr->color_parms.primaries[2][1] / 65536.0;
            // hdr_mdcv.white_point_chromaticity_* values are in 0.16 fixed-point format.
            //so we will shift right 16bit change to float 0.16 farmat value.
            hdr.mastering.white.x	= 	phdr->color_parms.white_point[0] / 65536.0;
            hdr.mastering.white.y	= 	phdr->color_parms.white_point[1] / 65536.0;
            // hdr_mdcv.luminance_max is in 24.8 fixed-point format.
            //so we will shift right 8bit change to float 24.8 farmat value.
            int32_t MaxDisplayLuminance = 0;
            MaxDisplayLuminance = phdr->color_parms.luminance[0];
            hdr.mastering.maxLuminance = (float)MaxDisplayLuminance / 256.0;
            // hdr_mdcv.luminance_min is in 18.14 format.
            //so we will shift right 14bit change to float 18.14 farmat value.
            hdr.mastering.minLuminance = phdr->color_parms.luminance[1] / 16384.0;
            hdr.maxCll =
                phdr->color_parms.content_light_level.max_content;
            hdr.maxFall =
                phdr->color_parms.content_light_level.max_pic_average;

        } else if ((mIntfImpl->getInputCodec() == InputCodec::H265)) {
            //see 265 spec
            // D.3.28 Mastering display colour volume SEI message semantics
            hdr.mastering.green.x	= 	phdr->color_parms.primaries[0][0] * 0.00002;
            hdr.mastering.green.y	= 	phdr->color_parms.primaries[0][1] * 0.00002;
            hdr.mastering.blue.x	=  	phdr->color_parms.primaries[1][0] * 0.00002;
            hdr.mastering.blue.y	= 	phdr->color_parms.primaries[1][1] * 0.00002;
            hdr.mastering.red.x     = 	phdr->color_parms.primaries[2][0] * 0.00002;
            hdr.mastering.red.y     = 	phdr->color_parms.primaries[2][1] * 0.00002;
            hdr.mastering.white.x	= 	phdr->color_parms.white_point[0] * 0.00002;
            hdr.mastering.white.y	= 	phdr->color_parms.white_point[1] * 0.00002;

            int32_t MaxDisplayLuminance = 0;
            MaxDisplayLuminance = min(50 * ((phdr->color_parms.luminance[0] + 250000) / 500000), 10000);
            hdr.mastering.maxLuminance = (float)MaxDisplayLuminance;

            hdr.mastering.minLuminance = phdr->color_parms.luminance[1] * 0.0001;
            hdr.maxCll =
                phdr->color_parms.content_light_level.max_content;
            hdr.maxFall =
                phdr->color_parms.content_light_level.max_pic_average;
        } else {
            hdr.mastering.green.x	= 	phdr->color_parms.primaries[0][0] * 0.00002;
            hdr.mastering.green.y	= 	phdr->color_parms.primaries[0][1] * 0.00002;
            hdr.mastering.blue.x	=  	phdr->color_parms.primaries[1][0] * 0.00002;
            hdr.mastering.blue.y	= 	phdr->color_parms.primaries[1][1] * 0.00002;
            hdr.mastering.red.x     = 	phdr->color_parms.primaries[2][0] * 0.00002;
            hdr.mastering.red.y     = 	phdr->color_parms.primaries[2][1] * 0.00002;
            hdr.mastering.white.x	= 	phdr->color_parms.white_point[0] * 0.00002;
            hdr.mastering.white.y	= 	phdr->color_parms.white_point[1] * 0.00002;

            int32_t MaxDisplayLuminance = 0;
            MaxDisplayLuminance = phdr->color_parms.luminance[0];
            hdr.mastering.maxLuminance = (float)MaxDisplayLuminance / 1000;

            hdr.mastering.minLuminance = phdr->color_parms.luminance[1] * 0.0001;
            hdr.maxCll =
                phdr->color_parms.content_light_level.max_content;
            hdr.maxFall =
                phdr->color_parms.content_light_level.max_pic_average;
        }

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
            C2VdecMDU_LOG(CODEC2_LOG_INFO, "Update color aspect p:%d/%d, r:%d/%d, m:%d/%d, t:%d/%d",
                        codedAspects.primaries, aspects.mPrimaries,
                        codedAspects.range, codedAspects.range,
                        codedAspects.matrix, aspects.mMatrixCoeffs,
                        codedAspects.transfer, aspects.mTransfer);
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            c2_status_t err = mIntfImpl->config({&codedAspects}, C2_MAY_BLOCK, &failures);
            if (err != C2_OK) {
                C2VdecMDU_LOG(CODEC2_LOG_ERR, "Failed to config hdr static info, error:%d", err);
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
            C2VdecMDU_LOG(CODEC2_LOG_ERR, "Failed to config hdr static info, error:%d", err);
        }
        std::lock_guard<std::mutex> lock(mMutex);
        mHDRStaticInfoChanged = true;
    }

    return 0;
}

int C2VdecComponent::DeviceUtil::checkHdrStaticInfoMetaChanged(struct aml_vdec_hdr_infos* phdr) {
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

    if (isHDRStaticInfoDifferent(&(mConfigParam->aml_dec_cfg.hdr), phdr)) {
        mConfigParam->aml_dec_cfg.hdr = *phdr;
        return true;
    }

    return false;
}

int C2VdecComponent::DeviceUtil::isHDRStaticInfoDifferent(struct aml_vdec_hdr_infos* phdr_old, struct aml_vdec_hdr_infos* phdr_new) {
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

int C2VdecComponent::DeviceUtil::getVideoType() {
    int videotype = AM_VIDEO_4K;//AM_VIDEO_AFBC;

    if (mConfigParam->aml_dec_cfg.cfg.double_write_mode == 0 ||
        mConfigParam->aml_dec_cfg.cfg.double_write_mode == 3) {
        if (mIntfImpl->getInputCodec() == InputCodec::VP9 ||
            mIntfImpl->getInputCodec() == InputCodec::H265 ||
            mIntfImpl->getInputCodec() == InputCodec::AV1 ||
            mIntfImpl->getInputCodec() == InputCodec::H264) {
            videotype |= AM_VIDEO_AFBC;
        }
    }

    return videotype;
}

int32_t C2VdecComponent::DeviceUtil::getPropertyDoubleWrite() {
    char value[PROPERTY_VALUE_MAX];
    property_get(C2_PROPERTY_VDEC_DOUBLEWRITE, value, "-1");
    int32_t doubleWrite = atoi(value);
    CODEC2_LOG(CODEC2_LOG_INFO, "get property double write:%d", doubleWrite);
    return doubleWrite;
}

uint64_t C2VdecComponent::DeviceUtil::getUsageFromDouleWrite(uint32_t doublewrite) {
    uint64_t usage = am_gralloc_get_video_decoder_full_buffer_usage();
    switch (doublewrite) {
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

    return usage;
}

bool C2VdecComponent::DeviceUtil::checkDvProfileAndLayer() {
    bool uselayer = false;
    C2StreamProfileLevelInfo::input inputProfile;
    mIntfImpl->query({&inputProfile}, {}, C2_MAY_BLOCK, nullptr);
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
        C2VdecMDU_LOG(CODEC2_LOG_INFO,"update input Codec profile to %d",codecType);
        if (inputProfile.profile == C2Config::PROFILE_DV_HE_04) {
            uselayer = true;
        }
        mIntfImpl->updateInputCodec(codecType);
    }
    return uselayer;
}

bool C2VdecComponent::DeviceUtil::isYcrcb420Stream() const {
    return (mStreamBitDepth == 0 || mStreamBitDepth == 8);
}

bool C2VdecComponent::DeviceUtil::isYcbcRP010Stream() const {
    //The current component supports the maximum size of 720*576 of 10 bit streams.
    //If the size exceeds this size, 8 bit buffer format will be used by default.
    if ((mStreamBitDepth == 10) && (mBufferWidth <= kMaxWidthP010 && mBufferHeight <= kMaxHeightP010)) {
        return true;
    }

    return false;
}

uint64_t C2VdecComponent::DeviceUtil::getPlatformUsage() {
    uint64_t usage = am_gralloc_get_video_decoder_full_buffer_usage();

    if (mUseSurfaceTexture || mNoSurface) {
        usage = am_gralloc_get_video_decoder_OSD_buffer_usage();
        uint64_t rawUsage = usage;
        if (isYcbcRP010Stream()) {
            usage = rawUsage | GRALLOC1_PRODUCER_USAGE_PRIVATE_3;
        }
        CODEC2_LOG(CODEC2_LOG_INFO,"[%s:%d] usage:%llx raw usage:%llx",__func__, __LINE__,
                    (unsigned long long)usage, (unsigned long long)rawUsage);
    } else {
        uint32_t doubleWrite = getDoubleWriteModeValue();
        if (mIs8k) {
            usage = am_gralloc_get_video_decoder_quarter_buffer_usage();
            C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Is 8k use 1/4 usage:%llx",__func__, __LINE__, (unsigned long long)usage);
        } else if (mForceFullUsage) {
            usage = am_gralloc_get_video_decoder_full_buffer_usage();
            C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Force use full usage:%llx",__func__, __LINE__, (unsigned long long)usage);
        } else {
            usage = getUsageFromDouleWrite(doubleWrite);
            CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d] get usage:%llx doule write:%d", __func__, __LINE__, (unsigned long long)usage, doubleWrite);
        }
    }

    return usage & C2MemoryUsage::PLATFORM_MASK;
}

uint32_t C2VdecComponent::DeviceUtil::getOutAlignedSize(uint32_t size, bool forceAlign) {
    if ((mSecure && mIntfImpl->getInputCodec() == InputCodec::H264) || forceAlign)
        return (size + OUTPUT_BUFS_ALIGN_SIZE - 1) & (~(OUTPUT_BUFS_ALIGN_SIZE - 1));
    return size;
}


bool C2VdecComponent::DeviceUtil::needAllocWithMaxSize() {
    bool realloc = true;
    bool debugrealloc = property_get_bool(C2_PROPERTY_VDEC_OUT_BUF_REALLOC, false);

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
            case InputCodec::VP9:
            case InputCodec::H265:
            case InputCodec::AV1:
                realloc = false;
                break;
            default:
                break;
        }
    }
    return realloc;
}

bool C2VdecComponent::DeviceUtil::checkReallocOutputBuffer(VideoFormat rawFormat,VideoFormat currentFormat,
                                                    bool *sizechange, bool *buffernumincrease) {
    bool realloc = false, frameSizeChanged = false;
    bool bufferNumChanged = false;

    if (currentFormat.mMinNumBuffers != rawFormat.mMinNumBuffers) {
        bufferNumChanged = true;
    }

    if (currentFormat.mCodedSize.width() != rawFormat.mCodedSize.width() ||
        currentFormat.mCodedSize.height() !=  rawFormat.mCodedSize.height()) {
        frameSizeChanged = true;
        *sizechange = true;
    }

    if ((currentFormat.mMinNumBuffers != 0 && rawFormat.mMinNumBuffers != 0) &&
        (currentFormat.mMinNumBuffers > rawFormat.mMinNumBuffers)) {
        *buffernumincrease = true;
    }

    if (mNoSurface) {
        realloc = (bufferNumChanged || frameSizeChanged);
    } else {
        realloc = frameSizeChanged;
    }

    C2VdecMDU_LOG(CODEC2_LOG_INFO, "[%s:%d] raw size:%s %d new size:%s %d realloc:%d",__func__, __LINE__,
        rawFormat.mCodedSize.ToString().c_str(), rawFormat.mMinNumBuffers,
        currentFormat.mCodedSize.ToString().c_str(),currentFormat.mMinNumBuffers, realloc);

    return realloc;
}

bool C2VdecComponent::DeviceUtil::getMaxBufWidthAndHeight(uint32_t& width, uint32_t& height) {
    bool support_4k = property_get_bool("ro.vendor.platform.support.4k", true);

    if (support_4k) {
        if (mIs8k) {
            width = kMaxWidth8k;
            height = kMaxHeight8k;
        } else {
            width = kMaxWidth4k;
            height = kMaxHeight4k;
        }
        if (mIntfImpl->getInputCodec() == InputCodec::H265 && mIsInterlaced) {
            width = kMaxWidth1080p;
            height = kMaxHeight1080p;
        }
    } else {
        if (height > width) {
            width = kMaxHeight1080p;
            height = kMaxWidth1080p;
        } else {
            width = kMaxWidth1080p;
            height = kMaxHeight1080p;
        }
    }
    return true;
}

bool C2VdecComponent::DeviceUtil::getUvmMetaData(int fd, unsigned char *data, int *size) {
    if (mUvmFd <= 0) {
        mUvmFd = amuvm_open();
        if (mUvmFd < 0) {
            C2VdecMDU_LOG(CODEC2_LOG_ERR, "Open uvm device fail.");
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

void C2VdecComponent::DeviceUtil::parseAndProcessMetaData(unsigned char *data, int size, C2Work& work) {
    struct aml_meta_head_s *meta_head;
    uint32_t offset = 0;
    uint32_t meta_magic = 0, meta_type = 0, meta_size = 0;

    if (data == NULL || size <= 0) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR,"Parse and process meta data failed");
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
            C2VdecMDU_LOG(CODEC2_LOG_ERR,"Get mate head error");
            break;
        }
        unsigned char buf[meta_size];
        memset(buf, 0, meta_size);
        if ((offset + AML_META_HEAD_SIZE + meta_size) > size) {
            C2VdecMDU_LOG(CODEC2_LOG_ERR,"Metadata oversize %u > %u, please check",
                    (unsigned int)(offset + AML_META_HEAD_SIZE + meta_size), (unsigned int)size);
            break;
        }

        memcpy(buf, (data + offset + AML_META_HEAD_SIZE), meta_size);
        offset = offset + AML_META_HEAD_SIZE + meta_size;
        meta_head = (struct aml_meta_head_s *)(&data[offset]);

        if (meta_type == UVM_META_DATA_VF_BASE_INFOS) {
            updateDurationUs(buf, meta_size);
        } else if (meta_type == UVM_META_DATA_HDR10P_DATA) {
            updateHDR10plusToWork(buf, meta_size, work);
        }
    }
}

void C2VdecComponent::DeviceUtil::updateHDR10plusToWork(unsigned char *data, int size, C2Work& work) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (size > 0) {
        mHDR10PLusInfoChanged = true;
        std::unique_ptr<C2StreamHdrDynamicMetadataInfo::output> hdr10PlusInfo =
            C2StreamHdrDynamicMetadataInfo::output::AllocUnique(size);
        hdr10PlusInfo->m.type_ = C2Config::HDR_DYNAMIC_METADATA_TYPE_SMPTE_2094_40;
        memcpy(hdr10PlusInfo->m.data, data, size);
        work.worklets.front()->output.configUpdate.push_back(std::move(hdr10PlusInfo));
    }
}
bool C2VdecComponent::DeviceUtil::getHDR10PlusData(std::string &data)
{
    if (!mHDR10PlusData.empty()) {
        data = std::move(mHDR10PlusData.front());
        mHDR10PlusData.pop();
        return true;
    }
    return false;
}

void C2VdecComponent::DeviceUtil::save_stream_info(uint64_t timestamp, int filledlen) {
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

void C2VdecComponent::DeviceUtil::check_stream_info() {
    if (mInPutWorkCount == 3 && mAmlStreamInfo.pts_0 == 0
            && mAmlStreamInfo.pts_1 == 0
            && mAmlStreamInfo.pts_2 == 0) {
        C2VdecMDU_LOG(CODEC2_LOG_INFO,"First 3 pts all 0, pts invalid, use default framerate");
        mInPtsInvalid = true;
        return;
    }
}

void C2VdecComponent::DeviceUtil::updateDurationUs(unsigned char *data, int size) {
    uint32_t durationData = 0;
    if (data == NULL || size <= 0) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR,"Update DurationUs error");
        return;
    }

    struct aml_vf_base_info_s *baseinfo = (struct aml_vf_base_info_s *)(data);

    if (baseinfo != NULL && baseinfo->duration != 0) {
        durationData = baseinfo->duration;
        if (durationData != 0) {
            uint64_t rate64 = 1000000;
            rate64 = rate64 / (96000 * 1.0 / durationData);
            uint32_t dur = 0, oldDur = mDurationUs;
            if (mIsInterlaced)
                dur = 2 * rate64;
            else
                dur = rate64;
            mCredibleDuration = true;

            float durStep = std::max(mDurationUsFromApp, dur) / (float)min(mDurationUsFromApp, dur);
            if (mDurationUsFromApp > 0 && dur > 0 && (durStep < 1.3f)) {
                mDurationUs = dur;
            } else {
                mDurationUs = mDurationUsFromApp;
            }
            if (oldDur != mDurationUs)
                setDuration();
            C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Update DurationUs:%d DurationUsFromApp:%d Dur:%d %f by meta data", mDurationUs, mDurationUsFromApp, dur, durStep);
        }
    }
}

bool C2VdecComponent::DeviceUtil::updateDisplayInfoToGralloc(const native_handle_t* handle, int videoType, uint32_t sequenceNum) {
    if (mUseSurfaceTexture|| mNoSurface) {
        //Only set for surfaceview with hwc.
        return false;
    }

    if (am_gralloc_is_valid_graphic_buffer(handle)) {
        C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] mCurInstanceID:%d", __func__, sequenceNum);
        am_gralloc_set_omx_video_type(handle, videoType);
        am_gralloc_set_ext_attr(handle, GRALLOC_BUFFER_ATTR_AM_OMX_BUFFER_SEQUENCE, sequenceNum);
        return true;
    }

    return false;
}

bool C2VdecComponent::DeviceUtil::checkConfigInfoFromDecoderAndReconfig(int type) {
    bool ret = true;
    bool configChanged = false;

    //check whether need reconfig
    struct aml_dec_params *params = &mConfigParam->aml_dec_cfg;
    InputCodec codec = mIntfImpl->getInputCodec();

    if (type & INTERLACE) {
        if (codec == InputCodec::H265 && mIsInterlaced && params->cfg.double_write_mode == 0x03) {
           params->cfg.double_write_mode = 0x10;
           configChanged = true;
        }
    } else if (type & DOUBLE_WRITE) {
        if (mComp->isSecureMode() || mComp->isTunnelMode() || mComp->isAmDolbyVision()) {
            params->cfg.double_write_mode = 3;
            configChanged = true;
        }

        if (isYcbcRP010Stream()) {
            if (mComp->isNonTunnelMode() & (mUseSurfaceTexture || mNoSurface)) {
                params->cfg.double_write_mode = 3;
                configChanged = true;
            }
        }
    }

    if (configChanged) {
        AmlMessageBase *msg = VideoDecWraper::AmVideoDec_getAmlMessage();
        VideoDecWraper *videoWraper = mComp->getCompVideoDecWraper();
        if (msg != NULL && videoWraper != NULL) {
            msg->setPointer("reconfig", (void*)&mConfigParam);
            if (!videoWraper->postAndReplyMsg(msg)) {
                C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] set config to decoder failed!, please check", __func__, __LINE__);
            }
        } else {
            C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] set config to decoder failed!, please check", __func__, __LINE__);
        }
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "[%s:%d] reconfig decoder", __func__, __LINE__);

        if (msg != NULL)
            delete msg;
    }

    return ret;
}

bool C2VdecComponent::DeviceUtil::shouldEnableMMU() {
    if ((mIntfImpl->getInputCodec() == InputCodec::H264) && mEnableAvc4kMMU) {
        C2StreamPictureSizeInfo::output output;
        c2_status_t err = mIntfImpl->query({&output}, {}, C2_MAY_BLOCK, nullptr);
        if (err != C2_OK)
            C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] Query PictureSize error for avc 4k mmu", __func__, __LINE__);
        else if(mUseSurfaceTexture)
            C2VdecMDU_LOG(CODEC2_LOG_INFO, "mUseSurfaceTexture = %d, DO NOT Enable MMU", mUseSurfaceTexture);
        else if (output.width * output.height >= 3840 * 2160) {
            C2VdecMDU_LOG(CODEC2_LOG_INFO, "4k H264 Stream use MMU, width:%d height:%d",
                      output.width, output.height);
            return true;
        }
    }
    return false;
}

}
