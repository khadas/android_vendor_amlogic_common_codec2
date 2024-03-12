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

#define LOG_NDEBUG 0
#define LOG_TAG "C2VdecDeviceUtil"

#include <stdio.h>
#include <stdlib.h>
#include <utils/Log.h>
#include <Codec2Mapper.h>
#include <cutils/properties.h>
#include <SystemControlClient.h>

#include <C2VdecDeviceUtil.h>
#include <C2VdecInterfaceImpl.h>
#include <VideoDecodeAcceleratorAdaptor.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <media/stagefright/foundation/hexdump.h>
#include <am_gralloc_ext.h>
#include <C2VendorProperty.h>
#include <C2VendorDebug.h>
#include <C2VendorConfig.h>
#include <C2VdecCodecConfig.h>
#include <grallocwraper/GrallocWraper.h>
#include <inttypes.h>

#define V4L2_PARMS_MAGIC 0x55aacc33

#define C2VdecMDU_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d#%d]"#fmt, comp->mSessionID, comp->mDecoderID, ##str)

#define OUTPUT_BUFS_ALIGN_SIZE_32 (32)
#define OUTPUT_BUFS_ALIGN_SIZE_64 (64)
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

C2VdecComponent::DeviceUtil::DeviceUtil(bool secure) {
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    init(secure);
    CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
}

C2VdecComponent::DeviceUtil::~DeviceUtil() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
    if (mHdr10PlusInfo != nullptr) {
        mHdr10PlusInfo.reset();
    }
    property_set(C2_PROPERTY_COMMON_LOWLATENCY_MODE, "0");
    CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d] clear %s", __func__, __LINE__, C2_PROPERTY_COMMON_LOWLATENCY_MODE);
}

void C2VdecComponent::DeviceUtil::init(bool secure) {
    // config
    mSignalType = 0;
    mBufferWidth = 0;
    mBufferHeight = 0;
    mDecoderWidthAlign = -1;
    mOutputWorkCount = 0;
    mDurationUsFromApp = 0;
    mConfigParam = NULL;
    mSecure = secure;

    // P010
    mStreamBitDepth = -1;
    mIsYcbRP010Stream = false;
    mIsNeedUse10BitOutBuffer = false;
    mHwSupportP010 = property_get_bool(PROPERTY_PLATFORM_SUPPORT_HARDWARE_P010, false);
    mSwSupportP010 = property_get_bool(PROPERTY_PLATFORM_SUPPORT_SOFTWARE_P010, true);
    mUseP010ForDisplay = false;

    mDiPost = property_get_bool(C2_PROPERTY_VDEC_DI_POST, false);
    // 8K
    mIs8k = false;
    mEnable8kNR = false;

    // NR DI
    mEnableNR = false;
    mNoSurface = false;
    mIsInterlaced = false;
    mEnableAvc4kMMU = false;
    mAVCMMUWidth = 2560;
    mAVCMMUHeight = 2160;
    mForceFullUsage = false;
    mBufMode = DMA_BUF_MODE;
    mEnableDILocalBuf = false;
    mUseSurfaceTexture = false;
    mForceDIPermission = false;
    mColorAspectsChanged = false;
    mDisableErrPolicy = true;

    // low-latency mode
    mUseLowLatencyMode = false;

    // HDR
    mHDRStaticInfoChanged = false;
    mHDR10PLusInfoChanged = false;
    mHaveHdr10PlusInStream = false;

    mEnableAdaptivePlayback = false;
    mPlayerId = 0;

    // PTS
    mLastOutPts = 0;
    mDurationUs = 0;
    mFramerate = 0.0f;
    mUnstablePts = 0;
    mCredibleDuration = 0;
    mMarginBufferNum = 0;

    // gralloc
    mGrallocWraper = std::make_unique<GrallocWraper>();

    // For Game Mode
    mMemcMode = 0;
}

c2_status_t C2VdecComponent::DeviceUtil::setComponent(std::shared_ptr<C2VdecComponent> sharedcomp) {
    mComp = sharedcomp;
    std::shared_ptr<C2VdecComponent::IntfImpl> intfImpl = sharedcomp->GetIntfImpl();
    mIntfImpl = intfImpl;
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d][%s:%d]", sharedcomp->mSessionID, sharedcomp->mDecoderID, __func__, __LINE__);
    mGrallocWraper->setComponent(sharedcomp);

    paramsPreCheck(intfImpl);

    return C2_OK;
}

int32_t C2VdecComponent::DeviceUtil::getDoubleWriteModeValue() {
    uint32_t doubleWriteValue = 3;
    LockWeakPtrWithReturnVal(comp, mComp, doubleWriteValue);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, doubleWriteValue);

    InputCodec codec = intfImpl->getInputCodec();
    int32_t defaultDoubleWrite = getPropertyDoubleWrite();
    int32_t fixedBufferSlice = property_get_int32(C2_PROPERTY_VDEC_FIXED_BUFF_SLICE, -1);

    if (defaultDoubleWrite >= 0) {
        doubleWriteValue = defaultDoubleWrite;
        CODEC2_LOG(CODEC2_LOG_INFO, "set double write(%d) from property", doubleWriteValue);
        return doubleWriteValue;
    }

    if (mIs8k && !C2VdecCodecConfig::getInstance().isDisplaySupport8k()) {
        doubleWriteValue = 4;
        return doubleWriteValue;
    }

    if (fixedBufferSlice != 540 && comp->isAmDolbyVision() && property_get_bool(C2_PROPERTY_VDEC_AMDV_USE_540P, false)) {
        fixedBufferSlice = 540;
    }

    switch (codec) {
        case InputCodec::DVAV:
        case InputCodec::H264:
            if ((comp->isNonTunnelMode() && (mUseSurfaceTexture || mNoSurface)) ||
                    mIsInterlaced || !mEnableNR || !mEnableDILocalBuf ||
                    mDiPost) {
                doubleWriteValue = 0x10;
                if ((fixedBufferSlice == 540) && (!mUseSurfaceTexture && !mNoSurface && !mIsInterlaced)) {
                    doubleWriteValue = 0x400;
                    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "540p_buffer enabled, set avc double write %d",doubleWriteValue);
                }
                CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s-%d] texture/nosurface or interlaced or no di/nr video use dw %d", __func__, __LINE__, doubleWriteValue);
            } else {
                if ((fixedBufferSlice == 540) && !mUseSurfaceTexture && !mSecure) { // fix 540p buffer.
                    doubleWriteValue = 0x400;
                    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s-%d] 540p_buffer enabled, set avc double write %d", __func__, __LINE__, doubleWriteValue);
                } else if ((fixedBufferSlice == 1080) && !mSecure) {  // fix 1080p buffer.
                    doubleWriteValue = 0x200;
                } else {
                    doubleWriteValue = 3;
                }
            }
            break;
        case InputCodec::MP2V:
        case InputCodec::MP4V:
        case InputCodec::MJPG:
            doubleWriteValue = 0x10;
            CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "codec is mp2v/mp4v/mjpg, set double write %d", doubleWriteValue);
            break;
        case InputCodec::H265:
        case InputCodec::VP9:
        case InputCodec::AV1:
        case InputCodec::DVAV1:
        case InputCodec::DVHE:
            if (mHwSupportP010 && mIsYcbRP010Stream) {
                if (comp->isNonTunnelMode() && (mUseSurfaceTexture || mNoSurface)) {
                    doubleWriteValue = 0x10001;
                    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "surface texture/nosurface use dw:%d", doubleWriteValue);
                } else if (mUseP010ForDisplay) {
                    doubleWriteValue = 0x10003;
                    if ((fixedBufferSlice == 1080) && !mSecure) {
                        doubleWriteValue = 0x10200;
                    }
                } else {
                    doubleWriteValue = 3;
                }

                CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s-%d] surfaceuse dw:%d", __func__, __LINE__, doubleWriteValue);
            } else if (comp->isNonTunnelMode() && (mUseSurfaceTexture || mNoSurface)) {
                doubleWriteValue = 1;
                if (mIsYcbRP010Stream) {
                    doubleWriteValue = 3;
                }
                CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "surface texture/nosurface use dw:%d", doubleWriteValue);
            } else if (codec == InputCodec::H265 && mIsInterlaced) {
                doubleWriteValue = 1;
            } else {
                if ((fixedBufferSlice == 540) &&
                    !mUseSurfaceTexture && !mSecure) { // fix 540p buffer.
                    doubleWriteValue = 0x400;
                    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "540p_buffer enabled, set double write %d", doubleWriteValue);
                } else if ((fixedBufferSlice == 1080) && !mSecure) { // fix 1080p buffer.
                    doubleWriteValue = 0x200;
                } else {
                    doubleWriteValue = 3;
                }
            }
            break;
        case InputCodec::AVS2:
            doubleWriteValue = 3;
            break;
        case InputCodec::AVS:
            doubleWriteValue = 0x10;
            break;
        case InputCodec::AVS3:
            doubleWriteValue = 3;
            break;
        case InputCodec::VC1:
            doubleWriteValue = 0x10;
            break;
        default:
            doubleWriteValue = 3;
            break;
    }

    if (shouldEnableMMU()) {
        doubleWriteValue = 3;
        CODEC2_LOG(CODEC2_LOG_INFO, "H264 4k mmu :DoubleWrite %d", doubleWriteValue);
        return doubleWriteValue;
    }

    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "component double write value:%d", doubleWriteValue);
    return doubleWriteValue;
}

int32_t C2VdecComponent::DeviceUtil::getTripleWriteModeValue() {
    int32_t tripleWriteValue = 0x10001;
    LockWeakPtrWithReturnVal(comp, mComp, tripleWriteValue);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, tripleWriteValue);

    InputCodec codec = intfImpl->getInputCodec();
    int32_t defaultTripleWrite = getPropertyTripleWrite();
    if (defaultTripleWrite >= 0) {
        tripleWriteValue = defaultTripleWrite;
        CODEC2_LOG(CODEC2_LOG_INFO, "set double write(%d) from property", tripleWriteValue);
        return tripleWriteValue;
    }

    switch (codec) {
        case InputCodec::H264:
            if ((comp->isNonTunnelMode() && (mUseSurfaceTexture || mNoSurface))) {
                tripleWriteValue = 0x10001;
                CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s-%d]texture/nosurface video use tw:0x%llx", __func__, __LINE__, (unsigned long long)tripleWriteValue);
            } else {
                tripleWriteValue = 0x10001;
            }
            break;
        case InputCodec::H265:
        case InputCodec::VP9:
        case InputCodec::AV1:
            if (comp->isNonTunnelMode() && (mUseSurfaceTexture || mNoSurface)) {
                tripleWriteValue = 0x10001;
                CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s-%d]texture/nosurface video use tw:0x%llx", __func__, __LINE__, (unsigned long long)tripleWriteValue);
            } else {
                tripleWriteValue = 0x10001;
            }
            break;
        default:
            tripleWriteValue = 0x10001;
            break;
    }

    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s-%d]component triple write value:%d", __func__, __LINE__, tripleWriteValue);
    return tripleWriteValue;
}

void C2VdecComponent::DeviceUtil::queryStreamBitDepth() {
    LockWeakPtrWithReturnVoid(comp, mComp);
    mVideoDecWraper = comp->getCompVideoDecWraper();
    LockWeakPtrWithReturnVoid(wraper, mVideoDecWraper);

    int32_t bitdepth = -1;
    AmlMessageBase *msg = VideoDecWraper::AmVideoDec_getAmlMessage();
    if (msg != NULL) {
        msg->setInt32("bitdepth", bitdepth);
        wraper->postAndReplyMsg(msg);
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
    bool support_soft_10bit = property_get_bool(PROPERTY_PLATFORM_SUPPORT_SOFTWARE_P010, true);
    if (support_soft_10bit && (mIsYcbRP010Stream || mIsNeedUse10BitOutBuffer)) {
        format = HAL_PIXEL_FORMAT_YCBCR_P010;
    }

    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s-%d IsYcbRP010Stream:%d IsNeedYcbRP010OutBuffer:%d format:%d",__func__, __LINE__,
                    mIsYcbRP010Stream, mIsNeedUse10BitOutBuffer, format);
    return format;
}

int32_t C2VdecComponent::DeviceUtil::getDecoderWidthAlign() {
    LockWeakPtrWithReturnVal(comp, mComp, -1);
    mVideoDecWraper = comp->getCompVideoDecWraper();
    LockWeakPtrWithReturnVal(wraper, mVideoDecWraper, -1);

    if (mDecoderWidthAlign != -1) {
        C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1, "return queried decoder width align(%d) directly.", mDecoderWidthAlign);
        return mDecoderWidthAlign;
    }

    int32_t widthAlign = -1;
    AmlMessageBase *msg = VideoDecWraper::AmVideoDec_getAmlMessage();
    if (msg != NULL) {
        msg->setInt32("widthalign", widthAlign);
        wraper->postAndReplyMsg(msg);
        msg->findInt32("widthalign", &widthAlign);
        if (widthAlign == 32 || widthAlign == 64) {
            mDecoderWidthAlign = widthAlign;
            C2VdecMDU_LOG(CODEC2_LOG_INFO, "Query the decoder width align(%d) success.", widthAlign);
        } else {
            C2VdecMDU_LOG(CODEC2_LOG_ERR, "Query the decoder width align failed.");
        }
    }

    if (msg != NULL)
        delete msg;

    return mDecoderWidthAlign;
}

bool C2VdecComponent::DeviceUtil::paramsPreCheck(std::shared_ptr<C2VdecComponent::IntfImpl> intfImpl) {
    c2_status_t err = C2_OK;
    C2VendorPlayerId::input playerId = {0};
    err = intfImpl->query({&playerId}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        CODEC2_LOG(CODEC2_LOG_ERR, "[%s:%d] Query C2VendorPlayerId message error", __func__, __LINE__);
    } else {
        mPlayerId = playerId.value;
    }

    return true;
}

void C2VdecComponent::DeviceUtil::codecConfig(mediahal_cfg_parms* configParam) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    LockWeakPtrWithReturnVoid(intfImpl, mIntfImpl);

    uint32_t doubleWriteMode = 3;
    int default_margin = 6;
    uint32_t bufwidth = 0;
    uint32_t bufheight = 0;
    uint32_t margin = default_margin;
    bool dvUseTwoLayer = false;
    //default buf mode is dma
    mBufMode = DMA_BUF_MODE;
    if (configParam == NULL) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] codec config param error, please check.", __func__, __LINE__);
        return;
    }

    mEnableNR = property_get_bool(C2_PROPERTY_VDEC_DISP_NR_ENABLE, false);
    mEnableDILocalBuf = property_get_bool(C2_PROPERTY_VDEC_DISP_DI_LOCALBUF_ENABLE, false);
    mEnable8kNR = property_get_bool(C2_PROPERTY_VDEC_DISP_NR_8K_ENABLE, false);
    mDisableErrPolicy = property_get_bool(C2_PROPERTY_VDEC_ERRPOLICY_DISABLE, true);
    mForceDIPermission = property_get_bool(C2_PROPERTY_VDEC_FORCE_DI_PERMISSION, false);

    mConfigParam = configParam;
    memset(mConfigParam, 0, sizeof(mediahal_cfg_parms));
    struct v4l2_parms * pAmlV4l2Param    = &mConfigParam->v4l2_cfg;
    struct aml_dec_params * pAmlDecParam = &mConfigParam->aml_dec_cfg;

    C2StreamPictureSizeInfo::output output = {0};
    c2_status_t err = C2_OK;
    err = intfImpl->query({&output}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] Query C2StreamPictureSizeInfo size error", __func__, __LINE__);
    }

    C2StreamFrameRateInfo::input inputFrameRateInfo = {0};
    err = intfImpl->query({&inputFrameRateInfo}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] Query C2StreamFrameRateInfo message error", __func__, __LINE__);
    }

    C2StreamUnstablePts::input unstablePts = {0};
    err = intfImpl->query({&unstablePts}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] Query C2StreamUnstablePts message error", __func__, __LINE__);
    } else {
        mUnstablePts = unstablePts.enable;
    }

    if (inputFrameRateInfo.value != 0) {
        mDurationUs = 1000 * 1000 / inputFrameRateInfo.value;
        mCredibleDuration = true;
        mFramerate = inputFrameRateInfo.value;
    }

    mDurationUsFromApp = mDurationUs;
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "[%s:%d] query frame rate:%f updata mDurationUs = %d, unstablePts :%d",__func__, __LINE__, inputFrameRateInfo.value, mDurationUs, mUnstablePts);
    C2GlobalLowLatencyModeTuning lowLatency = {0};
    err = intfImpl->query({&lowLatency}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] query C2GlobalLowLatencyModeTuning error", __func__, __LINE__);
    }

    if (lowLatency.value) {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "Config low latency mode to v4l2 decoder.");
        pAmlDecParam->cfg.low_latency_mode |= (LOWLATENCY_NORMAL|LOWLATENCY_FENCE);
        mUseLowLatencyMode = true;
        mEnableNR = false;
        mEnableDILocalBuf = false;
        property_set(C2_PROPERTY_COMMON_LOWLATENCY_MODE, "1");
        CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d] set %s", __func__, __LINE__, C2_PROPERTY_COMMON_LOWLATENCY_MODE);
    } else {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "Disable low latency mode to v4l2 decoder.");
        pAmlDecParam->cfg.low_latency_mode |= LOWLATENCY_DISABLE;
        property_set(C2_PROPERTY_COMMON_LOWLATENCY_MODE, "0");
        CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d] clear %s", __func__, __LINE__, C2_PROPERTY_COMMON_LOWLATENCY_MODE);
    }

    if (intfImpl->mVendorGameModeLatency->enable) {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "Config game latency mode to v4l2 decoder.");
        pAmlDecParam->cfg.low_latency_mode |= (LOWLATENCY_NORMAL|LOWLATENCY_FENCE);
        mUseLowLatencyMode = true;
        mEnableNR = false;
        mEnableDILocalBuf = false;
    }

    if (intfImpl->mAvc4kMMUMode->value ||
           property_get_bool(C2_PROPERTY_VDEC_ENABLE_AVC_4K_MMU, false)) {
        mEnableAvc4kMMU = true;
        propGetInt(C2_PROPERTY_VDEC_AVC_MMU_WIDTH, &mAVCMMUWidth);
        propGetInt(C2_PROPERTY_VDEC_AVC_MMU_HEIGHT, &mAVCMMUHeight);
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "mEnableAvc4kMMU = %d, mmu open width:%d, height:%d", mEnableAvc4kMMU, mAVCMMUWidth, mAVCMMUHeight);
    } else {
        mEnableAvc4kMMU = false;
    }

    if (comp->isAmDolbyVision()) {
        mUseP010ForDisplay = property_get_bool(C2_PROPERTY_VDEC_AMDV_USE_P010, false);
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
    if (bufwidth * bufheight > kMaxWidth4k * kMaxHeight4k) {
        default_margin = 5;
        mIs8k = true;
        if (!mEnable8kNR) {
            mEnableNR = false;
        }
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "[%s:%d] is 8k",__func__, __LINE__);
    }

    doubleWriteMode = getDoubleWriteModeValue();

    default_margin = property_get_int32(C2_PROPERTY_VDEC_MARGIN, default_margin);
    margin = default_margin;
    pAmlDecParam->cfg.canvas_mem_mode = 0;
    mMarginBufferNum = margin;
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "DoubleWriteMode %d, margin:%d \n", doubleWriteMode, margin);

    if (mUseSurfaceTexture || mNoSurface) {
        mEnableNR = false;
        mEnableDILocalBuf = false;
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "SurfaceText or noSurface unuse avbc out");
        pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_UNUSE_AVBC_OUT;
    }

    if (mEnableNR) {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "Enable NR");
        pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_NR_ENABLE;
    }

    if (mEnableDILocalBuf) {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "Enable DILocalBuf");
        pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DI_LOCALBUF_ENABLE;
    }
    if (mDiPost) {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "Enable DIPOST");
        pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DI_POST;
    }

    if (mBufMode == DMA_BUF_MODE) {
        pAmlDecParam->cfg.metadata_config_flag &= ~VDEC_CFG_FLAG_BUF_MODE;
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "Buf dma mode 0x%x", pAmlDecParam->cfg.metadata_config_flag);
    } else {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "Ion dma mode");
        pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_BUF_MODE;
    }

    C2ErrorPolicy::input errorPolicy;
    err = intfImpl->query({&errorPolicy}, {}, C2_MAY_BLOCK, nullptr);
    if (err != C2_OK) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] query error policy error", __func__, __LINE__);
    }
    if (mDisableErrPolicy || !errorPolicy.value) {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "C2 need disable error policy");
        pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DIS_ERR_POLICY;
    }

    if (mForceDIPermission) {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "force enable di on special streams");
        pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_FORCE_DI;
    }

    pAmlDecParam->cfg.uvm_hook_type = 2;
    if (/*!mSecureMode*/1) {
        //common param
        pAmlDecParam->cfg.init_height = bufwidth;
        pAmlDecParam->cfg.init_width = bufheight;
        pAmlDecParam->cfg.ref_buf_margin = margin;
        pAmlDecParam->cfg.double_write_mode = doubleWriteMode;
        pAmlDecParam->cfg.canvas_mem_endian = 0;
        pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_CFGINFO;

        int32_t tripleWrite = 0;
        int32_t defaultTripleWrite = getPropertyTripleWrite();
        if (defaultTripleWrite >= 0) {
            tripleWrite = defaultTripleWrite;
            CODEC2_LOG(CODEC2_LOG_INFO, "set triple write:%d from property", tripleWrite);
        }
        pAmlDecParam->cfg.triple_write_mode = tripleWrite;
        CODEC2_LOG(CODEC2_LOG_INFO, "[%s] set triple write:%d", __func__, pAmlDecParam->cfg.triple_write_mode);

        switch (intfImpl->getInputCodec()) {
            case InputCodec::H264:
            case InputCodec::H265:
                {
                    pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DV_NEGATIVE;
                }
                break;
            case InputCodec::VP9:
            case InputCodec::AV1:
                {
                    pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DV_NEGATIVE;
                    setHDRStaticInfo();
                }
                break;
            case InputCodec::DVHE:
                {
                    if (dvUseTwoLayer) {
                        pAmlDecParam->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DV_TWOLAYER;
                    }
                }
                break;
            default:
                C2VdecMDU_LOG(CODEC2_LOG_ERR,"input codec unknown!");
                break;
        }
    }
}


bool C2VdecComponent::DeviceUtil::setUnstable()
{
    LockWeakPtrWithReturnVal(comp, mComp, false);
    mVideoDecWraper = comp->getCompVideoDecWraper();
    LockWeakPtrWithReturnVal(wraper, mVideoDecWraper, false);
    bool ret = false;
    C2VdecMDU_LOG(CODEC2_LOG_INFO,"into set mUnstablePts = %d ", mUnstablePts);
    AmlMessageBase *msg = VideoDecWraper::AmVideoDec_getAmlMessage();
    if (msg != NULL) {
        msg->setInt32("unstable", mUnstablePts);
        wraper->postAndReplyMsg(msg);
        ret = true;
    }
    if (msg != NULL)
        delete msg;
    return ret;
}
//this only called when exit playback
bool C2VdecComponent::DeviceUtil::clearDecoderDuration()
{
    LockWeakPtrWithReturnVal(comp, mComp, false);
    mVideoDecWraper = comp->getCompVideoDecWraper();
    LockWeakPtrWithReturnVal(wraper, mVideoDecWraper, false);
    bool ret = false;
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "into clearDecoderDuration");
    AmlMessageBase *msg = VideoDecWraper::AmVideoDec_getAmlMessage();
    if (msg != NULL ) {
        msg->setInt32("duration", 0);
        msg->setInt32("type", 2);
        wraper->postAndReplyMsg(msg);
        ret = true;
    }
    if (msg != NULL) {
        delete msg;
    }
    return ret;
}

bool C2VdecComponent::DeviceUtil::setDuration()
{
    LockWeakPtrWithReturnVal(comp, mComp, false);
    mVideoDecWraper = comp->getCompVideoDecWraper();
    LockWeakPtrWithReturnVal(wraper, mVideoDecWraper, false);
    bool ret = false;
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "into set mDurationUs = %d ", mDurationUs);
    AmlMessageBase *msg = VideoDecWraper::AmVideoDec_getAmlMessage();
    if (msg != NULL && mDurationUs != 0) {
        msg->setInt32("duration", mDurationUs);
        msg->setFloat("framerate", mFramerate);
        msg->setInt32("type", 2);
        wraper->postAndReplyMsg(msg);
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

int C2VdecComponent::DeviceUtil::HDRInfoDataBLEndianInt(int value) {
    bool enable = property_get_bool(C2_PROPERTY_VDEC_HDR_LITTLE_ENDIAN_ENABLE, true);
    if (enable)
        return value;
    else
        return ((value & 0x00FF) << 8 ) | ((value & 0xFF00) >> 8);
}

int C2VdecComponent::DeviceUtil::setHDRStaticInfo() {
    LockWeakPtrWithReturnVal(comp, mComp, -1);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, -1);

    std::vector<std::unique_ptr<C2Param>> params;
    C2StreamHdrStaticInfo::output hdr = {0};
    bool hasHDRStaticInfo = false;
    bool isPresent = true;
    int32_t matrixCoeffs = 0;
    int32_t transfer = 0;
    int32_t primaries = 0;
    bool    range = false;

    if (intfImpl == NULL) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "set HDR satic info error");
        return -1;
    }
    c2_status_t err = intfImpl->query({&hdr}, {}, C2_DONT_BLOCK, &params);
    if (err != C2_OK) {
        C2VdecMDU_LOG(CODEC2_LOG_ERR, "Query hdr info error");
        return 0;
    }

    if (((int32_t)(hdr.mastering.red.x) == 0) &&
        ((int32_t)(hdr.mastering.red.y) == 0) &&
        ((int32_t)(hdr.mastering.green.x) == 0) &&
        ((int32_t)(hdr.mastering.green.y) == 0) &&
        ((int32_t)(hdr.mastering.blue.x) == 0) &&
        ((int32_t)(hdr.mastering.blue.y) == 0) &&
        ((int32_t)(hdr.mastering.white.x) == 0) &&
        ((int32_t)(hdr.mastering.white.y) == 0) &&
        ((int32_t)(hdr.mastering.maxLuminance) == 0) &&
        ((int32_t)(hdr.mastering.minLuminance) == 0) &&
        ((int32_t)(hdr.maxCll) == 0) &&
        ((int32_t)(hdr.maxFall) == 0)) { /* default val */
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "No hdr static info set");
    } else {
        hasHDRStaticInfo = true;
    }


    if (!mHDRStaticInfoColorAspects && !hasHDRStaticInfo)  {
        return 0;
    }

    struct aml_dec_params *pAmlDecParam = &mConfigParam->aml_dec_cfg;
    if (hasHDRStaticInfo) {
        pAmlDecParam->hdr.color_parms.present_flag = 1;
        if ((intfImpl->getInputCodec() == InputCodec::AV1)) {
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
            pAmlDecParam->hdr.color_parms.luminance[0]    = (((int32_t)(hdr.mastering.maxLuminance * 256.0 + 0.5)));//info.sType1.mMaxDisplayLuminance
            // hdr_mdcv.luminance_min is in 18.14 format.
            //so we will shift left 14bit change to int farmat value.
            //The increase of 0.5 is to ensure that data will not be lost downward
            pAmlDecParam->hdr.color_parms.luminance[1]    = (int32_t)(hdr.mastering.minLuminance * 16384.0 + 0.5);//info.sType1.mMinDisplayLuminance;
            pAmlDecParam->hdr.color_parms.content_light_level.max_content     =  (int32_t)(hdr.maxCll);//info.sType1.mMaxContentLightLevel;
            pAmlDecParam->hdr.color_parms.content_light_level.max_pic_average =  (int32_t)(hdr.maxFall);//info.sType1.mMaxFrameAverageLightLevel;
        } else {
            pAmlDecParam->hdr.color_parms.primaries[0][0] = HDRInfoDataBLEndianInt(hdr.mastering.green.x / 0.00002 + 0.5);//info.sType1.mG.x;
            pAmlDecParam->hdr.color_parms.primaries[0][1] = HDRInfoDataBLEndianInt(hdr.mastering.green.y / 0.00002 + 0.5);//info.sType1.mG.y;
            pAmlDecParam->hdr.color_parms.primaries[1][0] = HDRInfoDataBLEndianInt(hdr.mastering.blue.x / 0.00002 + 0.5);//info.sType1.mB.x;
            pAmlDecParam->hdr.color_parms.primaries[1][1] = HDRInfoDataBLEndianInt(hdr.mastering.blue.y / 0.00002 + 0.5);//info.sType1.mB.y;
            pAmlDecParam->hdr.color_parms.primaries[2][0] = HDRInfoDataBLEndianInt(hdr.mastering.red.x / 0.00002 + 0.5);//info.sType1.mR.x;
            pAmlDecParam->hdr.color_parms.primaries[2][1] = HDRInfoDataBLEndianInt(hdr.mastering.red.y / 0.00002 + 0.5);//info.sType1.mR.y;
            pAmlDecParam->hdr.color_parms.white_point[0]  = HDRInfoDataBLEndianInt(hdr.mastering.white.x / 0.00002 + 0.5);//info.sType1.mW.x;
            pAmlDecParam->hdr.color_parms.white_point[1]  = HDRInfoDataBLEndianInt(hdr.mastering.white.y / 0.00002 + 0.5);//info.sType1.mW.y;
            pAmlDecParam->hdr.color_parms.luminance[0]    = HDRInfoDataBLEndianInt(((int32_t)(hdr.mastering.maxLuminance + 0.5)));//info.sType1.mMaxDisplayLuminance;
            pAmlDecParam->hdr.color_parms.luminance[1]    = HDRInfoDataBLEndianInt(hdr.mastering.minLuminance / 0.0001 + 0.5);//info.sType1.mMinDisplayLuminance;
            pAmlDecParam->hdr.color_parms.content_light_level.max_content     =  HDRInfoDataBLEndianInt(hdr.maxCll + 0.5);//info.sType1.mMaxContentLightLevel;
            pAmlDecParam->hdr.color_parms.content_light_level.max_pic_average =  HDRInfoDataBLEndianInt(hdr.maxFall + 0.5);//info.sType1.mMaxFrameAverageLightLevel;
        }
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "Set hdrstaticinfo: gx:%d gy:%d bx:%d by:%d rx:%d,ry:%d wx:%d wy:%d maxlum:%d minlum:%d maxcontent:%d maxpicAVG:%d, %f %f %f %f %f %f %f %f %f %f %f %f",
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
    }
    if (mHDRStaticInfoColorAspects) {
        ColorAspects sfAspects;
        memset(&sfAspects, 0, sizeof(sfAspects));
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
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "hdr.signal_type: 0x%x", pAmlDecParam->hdr.signal_type);
    }
    pAmlDecParam->parms_status |= V4L2_CONFIG_PARM_DECODE_HDRINFO;

    return 0;
}

void C2VdecComponent::DeviceUtil::updateDecParmInfo(aml_dec_params* pInfo) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    if (pInfo != NULL) {
        C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Parms status %x\n", pInfo->parms_status);
        if (pInfo->parms_status & V4L2_CONFIG_PARM_DECODE_HDRINFO) {
            checkHDRMetadataAndColorAspects(&pInfo->hdr);
        }
    }
}

void C2VdecComponent::DeviceUtil::updateInterlacedInfo(bool isInterlaced) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "[%s:%d] isInterlaced:%d", __func__, __LINE__, isInterlaced);
    mIsInterlaced = isInterlaced;
}

void C2VdecComponent::DeviceUtil::flush() {
    mLastOutPts = 0;
    mOutputWorkCount = 0;
}

int C2VdecComponent::DeviceUtil::checkHDRMetadataAndColorAspects(struct aml_vdec_hdr_infos* phdr) {
    LockWeakPtrWithReturnVal(comp, mComp, -1);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, -1);
    bool isHdrChanged = false;
    bool isColorAspectsChanged = false;
    C2StreamHdrStaticInfo::output hdr = {0};

    //setup hdr metadata, only present_flag is 1 there has a hdr metadata
    if (phdr->color_parms.present_flag == 1) {
        if ((intfImpl->getInputCodec() == InputCodec::VP9)) {
            hdr.mastering.green.x   =   phdr->color_parms.primaries[0][0] * 0.00002;
            hdr.mastering.green.y   =   phdr->color_parms.primaries[0][1] * 0.00002;
            hdr.mastering.blue.x    =   phdr->color_parms.primaries[1][0] * 0.00002;
            hdr.mastering.blue.y    =   phdr->color_parms.primaries[1][1] * 0.00002;
            hdr.mastering.red.x     =   phdr->color_parms.primaries[2][0] * 0.00002;
            hdr.mastering.red.y     =   phdr->color_parms.primaries[2][1] * 0.00002;
            hdr.mastering.white.x   =   phdr->color_parms.white_point[0] * 0.00002;
            hdr.mastering.white.y   =   phdr->color_parms.white_point[1] * 0.00002;

            hdr.mastering.maxLuminance = phdr->color_parms.luminance[0];

            hdr.mastering.minLuminance = phdr->color_parms.luminance[1] * 0.0001;
            hdr.maxCll =
                phdr->color_parms.content_light_level.max_content;
            hdr.maxFall =
                phdr->color_parms.content_light_level.max_pic_average;
        } else if ((intfImpl->getInputCodec() == InputCodec::AV1)) {
            //see 6.7.4. Metadata high dynamic range mastering display color volume
            //semantics
            //hdr_mdcv.primaries* values are in 0.16 fixed-point format.
            //so we will shift right 16bit change to float 0.16 farmat value.
            hdr.mastering.red.x     =   phdr->color_parms.primaries[0][0] / 65536.0;
            hdr.mastering.red.y     =   phdr->color_parms.primaries[0][1] / 65536.0;
            hdr.mastering.green.x   =   phdr->color_parms.primaries[1][0] / 65536.0;
            hdr.mastering.green.y   =   phdr->color_parms.primaries[1][1] / 65536.0;
            hdr.mastering.blue.x    =   phdr->color_parms.primaries[2][0] / 65536.0;
            hdr.mastering.blue.y    =   phdr->color_parms.primaries[2][1] / 65536.0;
            // hdr_mdcv.white_point_chromaticity_* values are in 0.16 fixed-point format.
            //so we will shift right 16bit change to float 0.16 farmat value.
            hdr.mastering.white.x   =   phdr->color_parms.white_point[0] / 65536.0;
            hdr.mastering.white.y   =   phdr->color_parms.white_point[1] / 65536.0;
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

        } else if ((intfImpl->getInputCodec() == InputCodec::H265)) {
            //see 265 spec
            // D.3.28 Mastering display colour volume SEI message semantics
            hdr.mastering.green.x   =   phdr->color_parms.primaries[0][0] * 0.00002;
            hdr.mastering.green.y   =   phdr->color_parms.primaries[0][1] * 0.00002;
            hdr.mastering.blue.x    =   phdr->color_parms.primaries[1][0] * 0.00002;
            hdr.mastering.blue.y    =   phdr->color_parms.primaries[1][1] * 0.00002;
            hdr.mastering.red.x     =   phdr->color_parms.primaries[2][0] * 0.00002;
            hdr.mastering.red.y     =   phdr->color_parms.primaries[2][1] * 0.00002;
            hdr.mastering.white.x   =   phdr->color_parms.white_point[0] * 0.00002;
            hdr.mastering.white.y   =   phdr->color_parms.white_point[1] * 0.00002;

            int32_t MaxDisplayLuminance = 0;
            MaxDisplayLuminance = min(50 * ((phdr->color_parms.luminance[0] + 250000) / 500000), 10000);
            hdr.mastering.maxLuminance = (float)MaxDisplayLuminance;
            hdr.mastering.minLuminance = phdr->color_parms.luminance[1] * 0.0001;
            hdr.maxCll =
                phdr->color_parms.content_light_level.max_content;
            hdr.maxFall =
                phdr->color_parms.content_light_level.max_pic_average;
        } else {
            hdr.mastering.green.x   =   phdr->color_parms.primaries[0][0] * 0.00002;
            hdr.mastering.green.y   =   phdr->color_parms.primaries[0][1] * 0.00002;
            hdr.mastering.blue.x    =   phdr->color_parms.primaries[1][0] * 0.00002;
            hdr.mastering.blue.y    =   phdr->color_parms.primaries[1][1] * 0.00002;
            hdr.mastering.red.x     =   phdr->color_parms.primaries[2][0] * 0.00002;
            hdr.mastering.red.y     =   phdr->color_parms.primaries[2][1] * 0.00002;
            hdr.mastering.white.x   =   phdr->color_parms.white_point[0] * 0.00002;
            hdr.mastering.white.y   =   phdr->color_parms.white_point[1] * 0.00002;

            hdr.mastering.maxLuminance = phdr->color_parms.luminance[0];
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
            memset(&aspects, 0, sizeof(aspects));
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
            c2_status_t err = intfImpl->config({&codedAspects}, C2_MAY_BLOCK, &failures);
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
        c2_status_t err = intfImpl->config({&hdr}, C2_MAY_BLOCK, &failures);
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
    LockWeakPtrWithReturnVal(comp, mComp, -1);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, -1);
    int videotype = AM_VIDEO_4K;//AM_VIDEO_AFBC;

    if (mConfigParam->aml_dec_cfg.cfg.double_write_mode == 0 ||
        mConfigParam->aml_dec_cfg.cfg.double_write_mode == 3) {
        if (intfImpl->getInputCodec() == InputCodec::VP9 ||
            intfImpl->getInputCodec() == InputCodec::H265 ||
            intfImpl->getInputCodec() == InputCodec::AV1 ||
            intfImpl->getInputCodec() == InputCodec::H264) {
            videotype |= AM_VIDEO_AFBC;
        }
    }

    return videotype;
}

bool C2VdecComponent::DeviceUtil::isHDRStaticInfoUpdated() {
    if (mHDRStaticInfoChanged) {
        std::lock_guard<std::mutex> lock(mMutex);
        mHDRStaticInfoChanged = false;
        return true;
    }

    return false;
}

bool C2VdecComponent::DeviceUtil::isHDR10PlusStaticInfoUpdated() {
    if (mHDR10PLusInfoChanged) {
        std::lock_guard<std::mutex> lock(mMutex);
        mHDR10PLusInfoChanged = false;
        return true;
    }

    return false;
}

bool C2VdecComponent::DeviceUtil::isColorAspectsChanged() {
    if (mColorAspectsChanged) {
        std::lock_guard<std::mutex> lock(mMutex);
        mColorAspectsChanged = false;
        return true;
    }

    return false;
}

int32_t C2VdecComponent::DeviceUtil::getPropertyDoubleWrite() {
    LockWeakPtrWithReturnVal(comp, mComp, -1);
    int32_t doubleWrite = property_get_int32(C2_PROPERTY_VDEC_DOUBLEWRITE, -1);
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "get property double write:%d", doubleWrite);
    return doubleWrite;
}

int32_t C2VdecComponent::DeviceUtil::getPropertyTripleWrite() {
    LockWeakPtrWithReturnVal(comp, mComp, -1);
    int32_t tripleWrite = property_get_int32(C2_PROPERTY_VDEC_TRIPLEWRITE, -1);
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "get property triple write:%d", tripleWrite);
    return tripleWrite;
}

bool C2VdecComponent::DeviceUtil::checkDvProfileAndLayer() {
    LockWeakPtrWithReturnVal(comp, mComp, false);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, false);
    bool uselayer = false;
    C2StreamProfileLevelInfo::input inputProfile = {0};
    c2_status_t err = intfImpl->query({&inputProfile}, {}, C2_MAY_BLOCK, nullptr);
    if (err == C2_OK) {
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
            C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1,"update input Codec profile to %d",codecType);
            if (inputProfile.profile == C2Config::PROFILE_DV_HE_04) {
                uselayer = true;
            }
            intfImpl->updateInputCodec(codecType);
        }
    }
    return uselayer;
}

bool C2VdecComponent::DeviceUtil::isYcrcb420Stream() const {
    return (mStreamBitDepth == 0 || mStreamBitDepth == 8);
}

uint32_t C2VdecComponent::DeviceUtil::checkUseP010Mode() {
    //The current component supports the maximum size of 720*576 of 10 bit streams.
    //If the size exceeds this size, 8 bit buffer format will be used by default.
    LockWeakPtrWithReturnVal(comp, mComp, false);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, false);

    uint32_t useP010Mode = kUnUseP010;

    //Use soft decoder support P010.
    if ((mStreamBitDepth == 10) && (mBufferWidth <= kMaxWidthP010 && mBufferHeight <= kMaxHeightP010)
        && (intfImpl->getPixelFormatInfoValue() != HAL_PIXEL_FORMAT_YCBCR_420_888) && !mSecure
        && (mUseSurfaceTexture || mNoSurface)) {
        mIsYcbRP010Stream = true;
        useP010Mode = kUseSoftwareP010;
    }

    //Use hardware decoder support P010.
    if ((mStreamBitDepth == 10) && mHwSupportP010
        && (intfImpl->getPixelFormatInfoValue() != HAL_PIXEL_FORMAT_YCBCR_420_888)) {
        mIsYcbRP010Stream = true;
        useP010Mode = kUseHardwareP010;
    }

    return useP010Mode;
}

bool C2VdecComponent::DeviceUtil::isUseVdecCore() {
    LockWeakPtrWithReturnVal(comp, mComp, false);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, false);
    bool isUseVdec = false;
    switch (intfImpl->getInputCodec()) {
            case InputCodec::MJPG:
            case InputCodec::H264:
            case InputCodec::MP2V:
            case InputCodec::MP4V:
            case InputCodec::AVS:
                isUseVdec = true;
                break;
            default:
                isUseVdec = false;
                break;
    }
    return isUseVdec;
}

bool C2VdecComponent::DeviceUtil::needDecoderReplaceBufferForDiPost() {
    if (mIsInterlaced && mDiPost && isUseVdecCore() &&
        !(mUseSurfaceTexture || mNoSurface))
        return true;
    return false;
}

uint64_t C2VdecComponent::DeviceUtil::getPlatformUsage(const media::Size& size) {
    uint64_t usage = 0;
    LockWeakPtrWithReturnVal(comp, mComp, usage);

    struct aml_dec_params *params = &mConfigParam->aml_dec_cfg;
    int32_t doubleWrite = params->cfg.double_write_mode;
    int32_t tripleWrite = params->cfg.triple_write_mode;

    if (doubleWrite & 0x10000 || tripleWrite & 0x10000) {
        mIsNeedUse10BitOutBuffer = true;
    }

    usage = mGrallocWraper->getPlatformUsage(this, size);
    return usage & C2MemoryUsage::PLATFORM_MASK;
}

bool C2VdecComponent::DeviceUtil::checkSupport8kMode() {
    //need report error for 8k video at not surface mode if device not support 8k buf mode.
    bool support = property_get_bool(PROPERTY_PLATFORM_SUPPORT_8K_BUF_MODE, false);
    if (mIs8k && (mUseSurfaceTexture || mNoSurface) && !support) {
        return false;
    }
    return true;
}

uint32_t C2VdecComponent::DeviceUtil::getOutAlignedSize(uint32_t size, bool align64, bool forceAlign) {
    LockWeakPtrWithReturnVal(comp, mComp, 0);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, 0);

    int align = OUTPUT_BUFS_ALIGN_SIZE_64;

    if (mDecoderWidthAlign == -1) {
        align = getDecoderWidthAlign();
    }

    if (align != OUTPUT_BUFS_ALIGN_SIZE_32 && align != OUTPUT_BUFS_ALIGN_SIZE_64) {
        align = OUTPUT_BUFS_ALIGN_SIZE_64;
    }
    if (align64 == true) {
        align = OUTPUT_BUFS_ALIGN_SIZE_64;
    }
    if ((mSecure && intfImpl->getInputCodec() == InputCodec::H264) || forceAlign)
        return (size + align - 1) & (~(align - 1));

    if (mNoSurface) {
        return (size + align - 1) & (~(align - 1));
    }
    //fixed cl:371156 regression
    if (intfImpl->getInputCodec() == InputCodec::H264
        && getDoubleWriteModeValue() == 3
        && mEnableAvc4kMMU) {
        return (size + align - 1) & (~(align - 1));
    }
    return size;
}

bool C2VdecComponent::DeviceUtil::isNeedMaxSizeForAvc(int32_t doubleWrite) {
    switch (doubleWrite) {
    case 0:
    case 1:
    case 0x10001:
    case 0x10:
    case 0x10010:
        return false;
    default:
        return true;
    }
}

bool C2VdecComponent::DeviceUtil::needAllocWithMaxSize() {
    bool needMaxSize = false;
    LockWeakPtrWithReturnVal(comp, mComp, needMaxSize);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, needMaxSize);
    bool debugrealloc = property_get_bool(C2_PROPERTY_VDEC_OUT_BUF_REALLOC, false);

    if (debugrealloc)
        return false;

    if (mUseSurfaceTexture|| mNoSurface) {
        needMaxSize = false;
    } else {
        switch (intfImpl->getInputCodec()) {
            case InputCodec::MJPG:
            case InputCodec::MP2V:
            case InputCodec::MP4V:
                needMaxSize = false;
                break;
            case InputCodec::H264:
                if (mIsInterlaced || !isNeedMaxSizeForAvc(getDoubleWriteModeValue())) {
                    needMaxSize = false;
                } else {
                    needMaxSize = true;
                }
                break;
            case InputCodec::VP9:
            case InputCodec::H265:
            case InputCodec::AV1:
                needMaxSize = true;
                break;
            default:
                break;
        }
    }
    return needMaxSize;
}

bool C2VdecComponent::DeviceUtil::isNeedHalfHeightBuffer() {
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, false);
    return mIsInterlaced && intfImpl->getInputCodec() == InputCodec::H265 && mDiPost;
}

bool C2VdecComponent::DeviceUtil::isReallocateOutputBuffer(VideoFormat rawFormat,VideoFormat currentFormat,
                                                    bool *sizechange, bool *buffernumincrease) {
    bool realloc = false, frameSizeChanged = false;
    bool bufferNumChanged = false;
    LockWeakPtrWithReturnVal(comp, mComp, !realloc);

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

    if (realloc) {
        releaseGrallocSlot();
    }

    C2VdecMDU_LOG(CODEC2_LOG_INFO, "[%s:%d] raw size:%s %d new size:%s %d realloc:%d",__func__, __LINE__,
        rawFormat.mCodedSize.ToString().c_str(), rawFormat.mMinNumBuffers,
        currentFormat.mCodedSize.ToString().c_str(),currentFormat.mMinNumBuffers, realloc);

    return realloc;
}

void C2VdecComponent::DeviceUtil::releaseGrallocSlot() {
    mGrallocWraper->freeSlotID();
}

bool C2VdecComponent::DeviceUtil::getMaxBufWidthAndHeight(uint32_t& width, uint32_t& height) {
    LockWeakPtrWithReturnVal(comp, mComp, false);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, false);
    bool support_4k = property_get_bool(PROPERTY_PLATFORM_SUPPORT_4K, true);
    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;
    do {
        if (support_4k) {
            if (mIs8k) {
                maxWidth = kMaxWidth8k;
                maxHeight = kMaxHeight8k;
                break;
            }
            //mpeg2 and mpeg4 default size is 1080p
            if ((intfImpl->getInputCodec() == InputCodec::MP2V ||
                intfImpl->getInputCodec() == InputCodec::MP4V)
                ) {
                if (width * height <= kMaxWidth1080p * kMaxHeight1080p) {
                    maxWidth = kMaxWidth1080p;
                    maxHeight = kMaxHeight1080p;
                } else if (width * height <= kMaxWidth4k * kMaxHeight4k) {
                    maxWidth = kMaxWidth4k;
                    maxHeight = kMaxHeight4k;
                }
            } else {
                maxWidth = kMaxWidth4k;
                maxHeight = kMaxHeight4k;
            }
            //264 and 265 interlace stream
            if ((intfImpl->getInputCodec() == InputCodec::H265 ||
                intfImpl->getInputCodec() == InputCodec::H264) && mIsInterlaced) {
                maxWidth = kMaxWidth1080p;
                maxHeight = kMaxHeight1080p;
            }
        } else {
            maxWidth = kMaxWidth1080p;
            maxHeight = kMaxHeight1080p;
        }
    } while (0);

    if (height > width && maxWidth > 0) {
        width = maxHeight;
        height = maxWidth;
    } else {
        width = maxWidth;
        height = maxHeight;
    }
    return true;
}

bool C2VdecComponent::DeviceUtil::getUvmMetaData(int fd, unsigned char *data, int *size) {
    LockWeakPtrWithReturnVal(comp, mComp, false);
    if (data == NULL || fd < 0)
        return false;

    int32_t meta_size = 0;
    AmlMessageBase *msg = VideoDecWraper::AmVideoDec_getAmlMessage();
    std::shared_ptr<VideoDecWraper> videoWraper = comp->getCompVideoDecWraper();
    if (msg != NULL && videoWraper != NULL) {
        msg->setInt32("uvm", 1);
        msg->setInt32("fd", fd);
        msg->setInt32("getmetadata", 1);
        msg->setPointer("data", (void*)data);
        if (videoWraper->postAndReplyMsg(msg) == true) {
            msg->findInt32("size", &meta_size);
            *size = meta_size;
            if (msg != NULL)
                delete msg;
            return true;
        }
    }

    if (msg != NULL)
        delete msg;

    C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] get meta data from decoder error, please check.", __func__, __LINE__);
    return false;
}

void C2VdecComponent::DeviceUtil::parseAndProcessMetaData(unsigned char *data, int size, C2Work& work) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    struct aml_meta_head_s *meta_head;
    uint32_t offset = 0;
    uint32_t meta_magic = 0, meta_type = 0, meta_size = 0;

    if (data == NULL || size <= 0) {
        C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1, "parse and process meta data failed, please check.");
        return;
    }
    meta_head = (struct aml_meta_head_s *)data;
    bool haveUpdateHDR10Plus = false;
    while ((offset + AML_META_HEAD_SIZE) < size) {
        meta_magic = meta_head->magic;
        meta_type  = meta_head->type;
        meta_size  = meta_head->data_size;
        if (meta_magic != META_DATA_MAGIC ||
            (meta_size > META_DATA_SIZE) ||
            (meta_size <= 0)) {
            C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1, "meta head size error, please check.");
            break;
        }
        unsigned char buf[meta_size];
        memset(buf, 0, meta_size);
        if ((offset + AML_META_HEAD_SIZE + meta_size) > size) {
            C2VdecMDU_LOG(CODEC2_LOG_ERR, "meta data oversize %u > %u, please check",
                    (unsigned int)(offset + AML_META_HEAD_SIZE + meta_size), (unsigned int)size);
            break;
        }

        memcpy(buf, (data + offset + AML_META_HEAD_SIZE), meta_size);
        offset = offset + AML_META_HEAD_SIZE + meta_size;
        meta_head = (struct aml_meta_head_s *)(&data[offset]);

        if (meta_type == UVM_META_DATA_HDR10P_DATA) {
            updateHDR10plusToWork(buf, meta_size, work);
            haveUpdateHDR10Plus = true;
        }
    }

    if (mHaveHdr10PlusInStream && !haveUpdateHDR10Plus && (mHdr10PlusInfo != nullptr)) {
        C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL2, "update Decoder HDR10+ info use last data, timestap:%lld ",
                                    (unsigned long long)work.input.ordinal.customOrdinal.peekull());
        work.worklets.front()->output.configUpdate.push_back(C2Param::Copy(*mHdr10PlusInfo.get()));
    }
}

void C2VdecComponent::DeviceUtil::updateHDR10plusToWork(unsigned char *data, int size, C2Work& work) {
    std::lock_guard<std::mutex> lock(mMutex);
    LockWeakPtrWithReturnVoid(comp, mComp);
    LockWeakPtrWithReturnVoid(intfImpl, mIntfImpl);
    C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL2, "update Decoder HDR10+ info timestap:%lld size:%d data:",
                                (unsigned long long)work.input.ordinal.customOrdinal.peekull(), size);
    if (size > 0) {
        mHDR10PLusInfoChanged = true;
        mHaveHdr10PlusInStream = true;
        std::unique_ptr<C2StreamHdrDynamicMetadataInfo::output> hdr10PlusInfo =
            C2StreamHdrDynamicMetadataInfo::output::AllocUnique(size);
        hdr10PlusInfo->m.type_ = C2Config::HDR_DYNAMIC_METADATA_TYPE_SMPTE_2094_40;
        memcpy(hdr10PlusInfo->m.data, data, size);

        if (gloglevel & CODEC2_LOG_DEBUG_LEVEL2) {
            AString tmp;
            hexdump(data, size, 4, &tmp);
            ALOGD("%s", tmp.c_str());
        }
        if (nullptr == mHdr10PlusInfo || !(*hdr10PlusInfo == *mHdr10PlusInfo)) {
            if (mHdr10PlusInfo != nullptr) {
                mHdr10PlusInfo.reset();
            }
            mHdr10PlusInfo = std::move(hdr10PlusInfo);
        }
    }
    work.worklets.front()->output.configUpdate.push_back(C2Param::Copy(*mHdr10PlusInfo.get()));
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

void C2VdecComponent::DeviceUtil::setHDRStaticColorAspects(std::shared_ptr<C2StreamColorAspectsInfo::output> coloraspect) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    mHDRStaticInfoColorAspects = NULL;

#if 0
    C2VdecMDU_LOG(CODEC2_LOG_INFO, "HDR Static ColorAspects primaries(%d vs %d) transfer(%d vs %d %d) matrix(%d vs %d %d)",
                coloraspect->primaries, C2Color::PRIMARIES_BT2020,
                coloraspect->transfer, C2Color::TRANSFER_ST2084, C2Color::TRANSFER_HLG,
                coloraspect->matrix, C2Color::MATRIX_BT2020, C2Color::MATRIX_BT2020_CONSTANT);
#endif

    bool checkPrimaries = (coloraspect->primaries == C2Color::PRIMARIES_BT2020);
    bool checkTransfer = ((coloraspect->transfer == C2Color::TRANSFER_ST2084) ||
                            (coloraspect->transfer == C2Color::TRANSFER_HLG));

    bool checkMatrix = ((coloraspect->matrix == C2Color::MATRIX_BT2020) ||
                        (coloraspect->matrix == C2Color::MATRIX_BT2020_CONSTANT));

    if (checkPrimaries && checkTransfer && checkMatrix) {
        mHDRStaticInfoColorAspects = coloraspect;
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "HDR Static Info ColorAspects set");
    } else {
        C2VdecMDU_LOG(CODEC2_LOG_INFO, "No HDR ColorAspects set");
    }
}


bool C2VdecComponent::DeviceUtil::updateDisplayInfoToGralloc(const native_handle_t* handle, int videoType, uint32_t sequenceNum) {
    LockWeakPtrWithReturnVal(comp, mComp, false);
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
    LockWeakPtrWithReturnVal(comp, mComp, false);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, false);
    bool ret = true;
    bool configChanged = false;

    //check whether need reconfig
    struct aml_dec_params *params = &mConfigParam->aml_dec_cfg;
    InputCodec codec = intfImpl->getInputCodec();

    if (type & INTERLACE) {
        if (codec == InputCodec::H265 && mIsInterlaced && params->cfg.double_write_mode == 0x03) {
            if (mStreamBitDepth == 10) {
                params->cfg.double_write_mode = 1;
            } else {
                params->cfg.double_write_mode = 0x10;
            }
            configChanged = true;
        } else if ((codec == InputCodec::H264 && mIsInterlaced && params->cfg.double_write_mode == 0x03) ||
            needDecoderReplaceBufferForDiPost()) {
            params->cfg.double_write_mode = 0x10;
            configChanged = true;
        }

        int32_t disableVppThreshold = 0;
        propGetInt(C2_PROPERTY_VDEC_DIABLE_VPP_THRES, &disableVppThreshold);
        LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, false);
        C2VendorVideoBitrate::input bitrate = {0};
        c2_status_t err = intfImpl->query({&bitrate}, {}, C2_MAY_BLOCK, nullptr);
        int32_t nBitrate = (err == C2_OK) ?  bitrate.value : 0;
        if (disableVppThreshold > 0 && nBitrate > 0 && nBitrate/1024/1024 > disableVppThreshold) {
            // High bitrate interlaced stream, bypass vpp to reduce bandwidth to avoid drop frames
            params->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DISABLE_DECODE_VPP;
            C2VdecMDU_LOG(CODEC2_LOG_INFO, "[%s#%d] high bitrate interlaced stream, DISABLE_VPP[%X], bitrate:%d, threshold:%d",
                    __func__, __LINE__, params->cfg.metadata_config_flag, nBitrate/1024/1024, disableVppThreshold);
            configChanged = true;
        }
    } else if (type & TUNNEL_UNDERFLOW) {
        if (!(params->cfg.metadata_config_flag & VDEC_CFG_FLAG_DYNAMIC_BYPASS_DI) &&
            comp->mTunnelUnderflow) {
            params->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DYNAMIC_BYPASS_DI;
            configChanged = true;
        } else if ((params->cfg.metadata_config_flag & VDEC_CFG_FLAG_DYNAMIC_BYPASS_DI) &&
                !comp->mTunnelUnderflow) {
            params->cfg.metadata_config_flag &= (~VDEC_CFG_FLAG_DYNAMIC_BYPASS_DI);
            configChanged = true;
        }
    } else if (type & YCBCR_P010_STREAM) {
        if (mHwSupportP010) {
            int32_t doubleWrite = getDoubleWriteModeValue();
            int32_t tripleWrite = 0;

            int32_t defaultDoubleWrite = getPropertyDoubleWrite();
            int32_t defaultTripleWrite = getPropertyTripleWrite();

            int32_t doubleWriteValue = (defaultDoubleWrite >= 0) ? defaultDoubleWrite : doubleWrite;
            int32_t tripleWriteValue = (defaultTripleWrite >= 0) ? defaultTripleWrite : tripleWrite;

            params->cfg.double_write_mode = doubleWriteValue;
            params->cfg.triple_write_mode = tripleWriteValue;

            // by pass vpp.
            bool bypass_vpp = property_get_bool(C2_PROPERTY_VDEC_DIABLE_BYPASS_VPP, true);
            if (bypass_vpp)
                params->cfg.metadata_config_flag |= VDEC_CFG_FLAG_DISABLE_DECODE_VPP;

            C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s-%d double write:%d triple write:%d metadata_config_flag:%d", __func__, __LINE__,
                        params->cfg.double_write_mode, params->cfg.triple_write_mode, params->cfg.metadata_config_flag);
            configChanged = true;
        }
        else if (comp->isNonTunnelMode()
            &&(mUseSurfaceTexture || mNoSurface)
            && (params->cfg.double_write_mode != 3)) {
            params->cfg.double_write_mode = 3;
            configChanged = true;
            C2VdecMDU_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s-%d double write:%d ", __func__, __LINE__,
                        params->cfg.double_write_mode);
        }
    }

    if (configChanged) {
        AmlMessageBase *msg = VideoDecWraper::AmVideoDec_getAmlMessage();
        std::shared_ptr<VideoDecWraper> videoWraper = comp->getCompVideoDecWraper();
        if (msg != NULL && videoWraper != NULL) {
            msg->setPointer("reconfig", (void*)mConfigParam);
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
    LockWeakPtrWithReturnVal(comp, mComp, false);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, false);
    if ((intfImpl->getInputCodec() == InputCodec::H264) && mEnableAvc4kMMU && (!mIsInterlaced)) {
        C2StreamPictureSizeInfo::output output = {0};
        c2_status_t err = intfImpl->query({&output}, {}, C2_MAY_BLOCK, nullptr);
        if (err != C2_OK)
            C2VdecMDU_LOG(CODEC2_LOG_ERR, "[%s:%d] Query PictureSize error for avc 4k mmu", __func__, __LINE__);
        else if(mUseSurfaceTexture || mNoSurface)
            C2VdecMDU_LOG(CODEC2_LOG_TAG_BUFFER, "mUseSurfaceTexture = %d/mNoSurface = %d, DO NOT Enable MMU", mUseSurfaceTexture, mNoSurface);
        else if (output.width * output.height >= mAVCMMUWidth * mAVCMMUHeight) {
            if (mSecure) {
                return false;
            }
            C2VdecMDU_LOG(CODEC2_LOG_TAG_BUFFER, "Large H264 Stream use MMU, width:%d height:%d",
                    output.width, output.height);
            return true;
        }
    }
    return false;
}


void C2VdecComponent::DeviceUtil::setGameMode(bool enable) {
    static SystemControlClient *sc = SystemControlClient::getInstance();

    CODEC2_LOG(CODEC2_LOG_INFO, "setGameMode:%d", enable);
    if (enable) {
        mMemcMode = sc->getMemcMode();
        sc->setMemcMode(0, 0);
        sc->setProperty(C2_PROPERTY_VDEC_GAME_LOW_LATENCY, "1");
    } else {
        sc->setProperty(C2_PROPERTY_VDEC_GAME_LOW_LATENCY, "0");
        sc->setMemcMode(mMemcMode, 0);
    }
}

bool C2VdecComponent::DeviceUtil::isLowLatencyMode() {
    LockWeakPtrWithReturnVal(comp, mComp, false);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, false);
    return mUseLowLatencyMode;
}
}
