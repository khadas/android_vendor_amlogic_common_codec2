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
#define LOG_TAG "C2VdecComponentIntfImpl"

#include <media/stagefright/MediaDefs.h>
#include <C2VendorSupport.h>
#include <C2VdecInterfaceImpl.h>
#include <C2VdecComponent.h>
#include <C2VendorProperty.h>
#include <C2VdecCodecConfig.h>
#include <c2logdebug.h>

#include <C2PlatformSupport.h>
#include <SimpleC2Interface.h>
#include <C2VendorConfig.h>

namespace android {

#define AM_SIDEBAND_HANDLE_NUM_INT (3)

// Use basic graphic block pool/allocator as default.
const C2BlockPool::local_id_t kDefaultOutputBlockPool = C2PlatformAllocatorStore::GRALLOC;//C2PlatformAllocatorStore::BUFFERQUEUE;

//No-Tunnel Mode
constexpr uint32_t kDefaultOutputDelay = 3;             // The output buffer margin during initialization.
                                                        // Will be updated during playback.
constexpr uint32_t kMaxOutputDelay = 64;                // Max output delay.
constexpr uint32_t kMaxInputDelay = 4;                // Max input delay for no-secure.
constexpr uint32_t kMaxInputDelaySecure = 4;          // Max input delay for secure.
constexpr uint32_t kMaxInputDelayStreamMode = 256;    // Max input delay for stream mode.
constexpr uint32_t kMaxPipeLineDelayStreamMode = 256;    // Max pipeline delay for stream mode.


//Tunnel Mode
constexpr uint32_t kDefaultOutputDelayTunnel = 10;      // Default output delay on tunnel mode.

constexpr static size_t kMaxInputBufferSize = 8 * 1024 * 1024;

constexpr char MEDIA_MIMETYPE_VIDEO_AVS[] = "video/avs";
constexpr char MEDIA_MIMETYPE_VIDEO_AVS2[] = "video/avs2";
constexpr char MEDIA_MIMETYPE_VIDEO_AVS3[] = "video/avs3";
constexpr char MEDIA_MIMETYPE_VIDEO_VC1[] = "video/vc1";

#define DEFINE_C2_DEFAULT_UNSTRICT_SETTER(s, n) \
    C2R C2VdecComponent::IntfImpl::n##Setter(bool mayBlock, C2P<s> &me) {\
    (void)mayBlock;\
    (void)me;\
    return C2R::Ok();\
}

#define C2_DEFAULT_UNSTRICT_SETTER(n) n##Setter

static InputCodec getInputCodecFromDecoderName(const C2String name){
    for (int i = 0; i < sizeof(gC2ComponentInputCodec) / sizeof(C2ComponentInputCodec); i++) {
        if (name == gC2ComponentInputCodec[i].compname)
            return gC2ComponentInputCodec[i].codec;
    }

    return InputCodec::UNKNOWN;
}

// static
C2R C2VdecComponent::IntfImpl::ProfileLevelSetter(bool mayBlock,
                                                 C2P<C2StreamProfileLevelInfo::input>& info) {
    (void)mayBlock;
    return info.F(info.v.profile)
            .validatePossible(info.v.profile)
            .plus(info.F(info.v.level).validatePossible(info.v.level));
}

// static
C2R C2VdecComponent::IntfImpl::SizeSetter(bool mayBlock,
                                         C2P<C2StreamPictureSizeInfo::output>& videoSize) {
    (void)mayBlock;
    // TODO: maybe apply block limit?
    return videoSize.F(videoSize.v.width)
            .validatePossible(videoSize.v.width)
            .plus(videoSize.F(videoSize.v.height).validatePossible(videoSize.v.height));
}

// static
template <typename T>
C2R C2VdecComponent::IntfImpl::DefaultColorAspectsSetter(bool mayBlock, C2P<T>& def) {
    (void)mayBlock;
    if (def.v.range > C2Color::RANGE_OTHER) {
        def.set().range = C2Color::RANGE_OTHER;
    }
    if (def.v.primaries > C2Color::PRIMARIES_OTHER) {
        def.set().primaries = C2Color::PRIMARIES_OTHER;
    }
    if (def.v.transfer > C2Color::TRANSFER_OTHER) {
        def.set().transfer = C2Color::TRANSFER_OTHER;
    }
    if (def.v.matrix > C2Color::MATRIX_OTHER) {
        def.set().matrix = C2Color::MATRIX_OTHER;
    }
    return C2R::Ok();
}

// static
C2R C2VdecComponent::IntfImpl::MergedColorAspectsSetter(
        bool mayBlock, C2P<C2StreamColorAspectsInfo::output>& merged,
        const C2P<C2StreamColorAspectsTuning::output>& def,
        const C2P<C2StreamColorAspectsInfo::input>& coded) {
    (void)mayBlock;
    // Take coded values for all specified fields, and default values for unspecified ones.
    merged.set().range = coded.v.range == RANGE_UNSPECIFIED ? def.v.range : coded.v.range;
    merged.set().primaries =
            coded.v.primaries == PRIMARIES_UNSPECIFIED ? def.v.primaries : coded.v.primaries;
    merged.set().transfer =
            coded.v.transfer == TRANSFER_UNSPECIFIED ? def.v.transfer : coded.v.transfer;
    merged.set().matrix = coded.v.matrix == MATRIX_UNSPECIFIED ? def.v.matrix : coded.v.matrix;
    return C2R::Ok();
}

C2R C2VdecComponent::IntfImpl::Hdr10PlusInfoInputSetter(bool mayBlock, C2P<C2StreamHdr10PlusInfo::input> &me) {
    (void)mayBlock;
    (void)me;  // TODO: validate
    return C2R::Ok();
}

C2R C2VdecComponent::IntfImpl::Hdr10PlusInfoOutputSetter(bool mayBlock, C2P<C2StreamHdr10PlusInfo::output> &me) {
    (void)mayBlock;
    (void)me;  // TODO: validate
    return C2R::Ok();
}

DEFINE_C2_DEFAULT_UNSTRICT_SETTER(C2StreamHdrDynamicMetadataInfo::input, HdrDynamicInfoInput)
DEFINE_C2_DEFAULT_UNSTRICT_SETTER(C2StreamHdrDynamicMetadataInfo::output, HdrDynamicInfoOutput)

C2R C2VdecComponent::IntfImpl::HdrStaticInfoSetter(bool mayBlock, C2P<C2StreamHdrStaticInfo::output> &me) {
    (void)mayBlock;
    (void)me;  // TODO: validate
    return C2R::Ok();
}

C2R C2VdecComponent::IntfImpl::LowLatencyModeSetter(bool mayBlock, C2P<C2GlobalLowLatencyModeTuning> &me) {
    (void)mayBlock;
    return me.F(me.v.value).validatePossible(me.v.value);
}

C2R C2VdecComponent::IntfImpl::OutSurfaceAllocatorIdSetter(bool mayBlock, C2P<C2PortSurfaceAllocatorTuning::output> &me) {
    (void)mayBlock;
    (void)me;  // TODO: validate
    return C2R::Ok();
}

C2R C2VdecComponent::IntfImpl::TunnelModeOutSetter(bool mayBlock, C2P<C2PortTunneledModeTuning::output> &me) {
    (void)mayBlock;
    (void)me;  // TODO: validate
    return C2R::Ok();
}
C2R C2VdecComponent::IntfImpl::TunnelHandleSetter(bool mayBlock, C2P<C2PortTunnelHandleTuning::output> &me) {
    (void)mayBlock;
    (void)me;  // TODO: validate
    return C2R::Ok();
}
C2R C2VdecComponent::IntfImpl::TunnelSystemTimeSetter(bool mayBlock, C2P<C2PortTunnelSystemTime::output> &me) {
    (void)mayBlock;
    (void)me;  // TODO: validate
    return C2R::Ok();
}
C2R C2VdecComponent::IntfImpl::StreamPtsUnstableSetter(bool mayBlock, C2P<C2StreamUnstablePts::input> &me) {
    (void)mayBlock;
    (void)me;  // TODO: validate
    return C2R::Ok();
}

//define some unstrict Setter
DEFINE_C2_DEFAULT_UNSTRICT_SETTER(C2VendorTunerHalParam::input, VendorTunerHalParam)
DEFINE_C2_DEFAULT_UNSTRICT_SETTER(C2StreamTunnelStartRender::output, TunnelStartRender)
DEFINE_C2_DEFAULT_UNSTRICT_SETTER(C2VendorTunerPassthroughTrickMode::input, VendorTunerPassthroughTrickMode)
DEFINE_C2_DEFAULT_UNSTRICT_SETTER(C2VendorTunerPassthroughWorkMode::input, VendorTunerPassthroughWorkMode)
DEFINE_C2_DEFAULT_UNSTRICT_SETTER(C2VendorNetflixVPeek::input, VendorNetflixVPeek)
DEFINE_C2_DEFAULT_UNSTRICT_SETTER(C2VendorTunerPassthroughEventMask::input, VendorTunerPassthroughEventMask)
DEFINE_C2_DEFAULT_UNSTRICT_SETTER(C2VendorGameModeLatency::input, VendorGameModeLatency)

c2_status_t C2VdecComponent::IntfImpl::config(
    const std::vector<C2Param*> &params, c2_blocking_t mayBlock,
    std::vector<std::unique_ptr<C2SettingResult>>* const failures,
    bool updateParams,
    std::vector<std::shared_ptr<C2Param>> *changes) {
    DCHECK(mComponent != NULL);

    C2InterfaceHelper::config(params, mayBlock, failures, updateParams, changes);

    if (mComponent == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "[%s##%d] component is null, config param error!", __func__, __LINE__);
        return C2_BAD_STATE;
    }

    for (C2Param* const param : params) {
        switch (param->coreIndex().coreIndex()) {
            case C2PortTunneledModeTuning::CORE_INDEX:
                onTunneledModeTuningConfigParam();
                break;
            case C2VendorTunerHalParam::CORE_INDEX:
                onVendorTunerHalConfigParam();
                break;
            case C2VendorTunerPassthroughTrickMode::CORE_INDEX:
                onTunerPassthroughTrickModeConfigParam();
                break;
            case C2VendorTunerPassthroughWorkMode::CORE_INDEX:
                onTunerPassthroughWorkModeConfigParam();
                break;
            case C2VendorTunerPassthroughEventMask::CORE_INDEX:
                onTunerPassthroughEventMaskConfigParam();
                break;
            case C2VendorGameModeLatency::CORE_INDEX:
                CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]config game mode latency",
                    mComponent->mSessionID, mComponent->mDecoderID);
                break;
            case C2VdecWorkMode::CORE_INDEX:
                onVdecWorkModeConfigParam();
                break;
            case C2DataSourceType::CORE_INDEX:
                onDataSourceTypeConfigParam();
                break;
            case C2StreamModeInputDelay::CORE_INDEX:
                onStreamModeInputDelayConfigParam();
                break;
            case C2StreamModePipeLineDelay::CORE_INDEX:
                onStreamModePipeLineDelayConfigParam();
                break;
              case C2StreamModeHwAvSyncId::CORE_INDEX:
                CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]config stream mode hwavsyncid  :%d",
                    mComponent->mSessionID, mComponent->mDecoderID, mStreamModeHwAvSyncId->value);
                if (mComponent) {
                    mComponent->onConfigureEsModeHwAvsyncId(mStreamModeHwAvSyncId->value);
                }
                break;
            case C2StreamTunnelStartRender::CORE_INDEX:
                onStreamTunnelStartRenderConfigParam();
                break;
            case C2StreamHdr10PlusInfo::CORE_INDEX:
                onStreamHdr10PlusInfoConfigParam();
                break;
            case C2StreamMaxBufferSizeInfo::CORE_INDEX:
                onStreamMaxBufferSizeInfoConfigParam();
                break;
            case C2StreamPictureSizeInfo::CORE_INDEX:
                if (onStreamPictureSizeConfigParam(failures, param) != C2_OK)
                    return C2_BAD_VALUE;
                break;
            case C2StreamFrameRateInfo::CORE_INDEX:
                if (onStreamFrameRateConfigParam(failures, param) != true)
                    return C2_BAD_VALUE;
                break;
            case C2VendorNetflixVPeek::CORE_INDEX:
                onNetflixVPeekConfigParam();
                break;
            case C2ErrorPolicy::CORE_INDEX:
                onErrorPolicyConfigParam();
                break;
            default:
                break;
        }
    }
    return C2_OK;
}

void C2VdecComponent::IntfImpl::getHdr10PlusBuf(uint8_t** pbuf, uint32_t* plen) {
    if (pbuf == NULL || plen == NULL) {
        return;
    }
    *pbuf = mHdrDynamicInfoInput->m.data;
    *plen = mHdrDynamicInfoInput->flexCount();
}

void C2VdecComponent::IntfImpl::updateHdr10PlusInfoToWork(C2Work& work) {
    work.worklets.front()->output.configUpdate.push_back(C2Param::Copy(*mHdrDynamicInfoOutput.get()));
}

void C2VdecComponent::IntfImpl::updateInputCodec(InputCodec videotype) {
    if (InputCodec::DVHE <= videotype && videotype <= InputCodec::DVAV1) {
        mInputCodec = videotype;
    }
}

C2VdecComponent::IntfImpl::IntfImpl(C2String name, const std::shared_ptr<C2ReflectorHelper>& helper)
      : C2InterfaceHelper(helper) {
    setDerivedInstance(this);

    std::string inputMime;
    mComponent = NULL;
    mInitStatus = C2_OK;
    mCodecProfile = media::VIDEO_CODEC_PROFILE_UNKNOWN;

    // TODO: use factory function to determine whether V4L2 stream or slice API is.
    mInputCodec = getInputCodecFromDecoderName(name);
    mSecureMode = name.find(".secure") != std::string::npos;
    //profile and level
    switch (mInputCodec) {
        case InputCodec::H264:
            inputMime = MEDIA_MIMETYPE_VIDEO_AVC;
            onAvcDeclareParam();
        break;
        case InputCodec::H265:
            inputMime = MEDIA_MIMETYPE_VIDEO_HEVC;
            onHevcDeclareParam();
        break;
        case InputCodec::VP9:
            inputMime = MEDIA_MIMETYPE_VIDEO_VP9;
            onVp9DeclareParam();
        break;
        case InputCodec::AV1:
            inputMime = MEDIA_MIMETYPE_VIDEO_AV1;
            onAv1DeclareParam();
        break;
        case InputCodec::DVHE:
            inputMime = MEDIA_MIMETYPE_VIDEO_DOLBY_VISION;
            onDvheDeclareParam();
        break;
        case InputCodec::DVAV:
            inputMime = MEDIA_MIMETYPE_VIDEO_DOLBY_VISION;
            onDvavDeclareParam();
        break;
        case InputCodec::DVAV1:
            inputMime = MEDIA_MIMETYPE_VIDEO_DOLBY_VISION;
            onDvav1DeclareParam();
        break;
        case InputCodec::MP2V:
            inputMime = MEDIA_MIMETYPE_VIDEO_MPEG2;
            onMp2vDeclareParam();
        break;
        case InputCodec::MP4V:
            inputMime = MEDIA_MIMETYPE_VIDEO_MPEG4;
            onMp4vDeclareParam();
        break;
        case InputCodec::MJPG:
            inputMime = MEDIA_MIMETYPE_VIDEO_MJPEG;
            onMjpgDeclareParam();
        break;
        case InputCodec::AVS3:
            inputMime = MEDIA_MIMETYPE_VIDEO_AVS3;
            onAvs3DeclareParam();
        break;
        case InputCodec::AVS2:
            inputMime = MEDIA_MIMETYPE_VIDEO_AVS2;
            onAvs2DeclareParam();
        break;
        case InputCodec::AVS:
            inputMime = MEDIA_MIMETYPE_VIDEO_AVS;
            onAvsDeclareParam();
        break;
        case InputCodec::VC1:
            inputMime = MEDIA_MIMETYPE_VIDEO_VC1;
            onVc1DeclareParam();
        break;
        default:
            CODEC2_LOG(CODEC2_LOG_ERR, "Invalid component name: %s", name.c_str());
            mInitStatus = C2_BAD_VALUE;
        return;
    }

    //HDR
    onHdrDeclareParam(helper);

    //base setting
    onApiFeatureDeclareParam();

    //frame rate
    onFrameRateDeclareParam();

    //unstable pts
    onUnstablePtsDeclareParam();

    //out delay
    onOutputDelayDeclareParam();

    //input delay
    onInputDelayDeclareParam();

    //pipe delay
    onPipelineDelayDeclareParam();

    //record buf depth
    onReorderBufferDepthDeclareParam();

    //tunnel mode
    onTunnelDeclareParam();

    //tunnel passthrough
    onTunerPassthroughDeclareParam();

    // game mode latency
    onGameModeLatencyDeclareParam();

    //buffer size
    onBufferSizeDeclareParam(inputMime.c_str());

    //buffer pool
    onBufferPoolDeclareParam();

    //ColorAspects
    onColorAspectsDeclareParam();

    //lowlatency
    onLowLatencyDeclareParam();

    //PixelFormat
    onPixelFormatDeclareParam();

    //Register vendor extend paras
    onVendorExtendParam();

    //enable avc 4k mmu
    onAvc4kMMUEnable();

}

void C2VdecComponent::IntfImpl::onAvcDeclareParam() {
    addParameter(
        DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
        .withDefault(new C2StreamProfileLevelInfo::input(
                0u, C2Config::PROFILE_AVC_MAIN, C2Config::LEVEL_AVC_4))
        .withFields(
            {
                C2F(mProfileLevel, profile)
                .oneOf({C2Config::PROFILE_AVC_BASELINE,
                    C2Config::PROFILE_AVC_CONSTRAINED_BASELINE,
                    C2Config::PROFILE_AVC_MAIN,
                    C2Config::PROFILE_AVC_HIGH,
                    C2Config::PROFILE_AVC_CONSTRAINED_HIGH}),
                C2F(mProfileLevel, level)
                .oneOf({C2Config::LEVEL_AVC_1, C2Config::LEVEL_AVC_1B,
                    C2Config::LEVEL_AVC_1_1, C2Config::LEVEL_AVC_1_2,
                    C2Config::LEVEL_AVC_1_3, C2Config::LEVEL_AVC_2,
                    C2Config::LEVEL_AVC_2_1, C2Config::LEVEL_AVC_2_2,
                    C2Config::LEVEL_AVC_3, C2Config::LEVEL_AVC_3_1,
                    C2Config::LEVEL_AVC_3_2, C2Config::LEVEL_AVC_4,
                    C2Config::LEVEL_AVC_4_1, C2Config::LEVEL_AVC_4_2,
                    C2Config::LEVEL_AVC_5, C2Config::LEVEL_AVC_5_1})
            })
    .withSetter(ProfileLevelSetter)
    .build());
}

void C2VdecComponent::IntfImpl::onHevcDeclareParam() {
    C2VendorCodec vendorCodec = C2VdecCodecConfig::getInstance().adaptorInputCodecToVendorCodec(mInputCodec);
    if (!C2VdecCodecConfig::getInstance().isCodecSupport8k(vendorCodec, mSecureMode)) {
        addParameter(
            DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(
                    0u, C2Config::PROFILE_HEVC_MAIN, C2Config::LEVEL_HEVC_HIGH_5_1))
            .withFields(
                {
                    C2F(mProfileLevel, profile)
                    .oneOf({C2Config::PROFILE_HEVC_MAIN,
                        C2Config::PROFILE_HEVC_MAIN_10,
                        C2Config::PROFILE_HEVC_MAIN_STILL,
                        C2Config::PROFILE_HEVC_MAIN_422_10}),
                    C2F(mProfileLevel, level)
                    .oneOf({C2Config::LEVEL_HEVC_MAIN_1,
                        C2Config::LEVEL_HEVC_MAIN_2, C2Config::LEVEL_HEVC_MAIN_2_1,
                        C2Config::LEVEL_HEVC_MAIN_3, C2Config::LEVEL_HEVC_MAIN_3_1,
                        C2Config::LEVEL_HEVC_MAIN_4, C2Config::LEVEL_HEVC_MAIN_4_1,
                        C2Config::LEVEL_HEVC_MAIN_5, C2Config::LEVEL_HEVC_MAIN_5_1,
                        C2Config::LEVEL_HEVC_HIGH_4, C2Config::LEVEL_HEVC_HIGH_4_1,
                        C2Config::LEVEL_HEVC_HIGH_5, C2Config::LEVEL_HEVC_HIGH_5_1})
                })
        .withSetter(ProfileLevelSetter)
        .build());
    } else {
        addParameter(
            DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(
                    0u, C2Config::PROFILE_HEVC_MAIN, C2Config::LEVEL_HEVC_HIGH_6_1))
            .withFields(
                {
                    C2F(mProfileLevel, profile)
                    .oneOf({C2Config::PROFILE_HEVC_MAIN,
                        C2Config::PROFILE_HEVC_MAIN_10,
                        C2Config::PROFILE_HEVC_MAIN_STILL,
                        C2Config::PROFILE_HEVC_MAIN_422_10}),
                    C2F(mProfileLevel, level)
                    .oneOf({C2Config::LEVEL_HEVC_MAIN_1,
                        C2Config::LEVEL_HEVC_MAIN_2, C2Config::LEVEL_HEVC_MAIN_2_1,
                        C2Config::LEVEL_HEVC_MAIN_3, C2Config::LEVEL_HEVC_MAIN_3_1,
                        C2Config::LEVEL_HEVC_MAIN_4, C2Config::LEVEL_HEVC_MAIN_4_1,
                        C2Config::LEVEL_HEVC_MAIN_5, C2Config::LEVEL_HEVC_MAIN_5_1,
                        C2Config::LEVEL_HEVC_HIGH_4, C2Config::LEVEL_HEVC_HIGH_4_1,
                        C2Config::LEVEL_HEVC_HIGH_5, C2Config::LEVEL_HEVC_HIGH_5_1,
                        C2Config::LEVEL_HEVC_HIGH_6, C2Config::LEVEL_HEVC_HIGH_6_1})
                })
        .withSetter(ProfileLevelSetter)
        .build());
    }
}

void C2VdecComponent::IntfImpl::onVp9DeclareParam() {
    C2VendorCodec vendorCodec = C2VdecCodecConfig::getInstance().adaptorInputCodecToVendorCodec(mInputCodec);
    if (!C2VdecCodecConfig::getInstance().isCodecSupport8k(vendorCodec, mSecureMode)) {
        addParameter(
            DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(
                    0u, C2Config::PROFILE_VP9_0, C2Config::LEVEL_VP9_5_1))
            .withFields(
            {
                C2F(mProfileLevel, profile)
                .oneOf({C2Config::PROFILE_VP9_0,
                    C2Config::PROFILE_VP9_2,
                    C2Config::PROFILE_VP9_3}),
                C2F(mProfileLevel, level)
                .oneOf({C2Config::LEVEL_VP9_1, C2Config::LEVEL_VP9_1_1,
                    C2Config::LEVEL_VP9_2, C2Config::LEVEL_VP9_2_1,
                    C2Config::LEVEL_VP9_3, C2Config::LEVEL_VP9_3_1,
                    C2Config::LEVEL_VP9_4, C2Config::LEVEL_VP9_4_1,
                    C2Config::LEVEL_VP9_5, C2Config::LEVEL_VP9_5_1})
            })
        .withSetter(ProfileLevelSetter)
        .build());
    } else {
        addParameter(
            DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(
                    0u, C2Config::PROFILE_VP9_0, C2Config::LEVEL_VP9_6_1))
            .withFields(
            {
                C2F(mProfileLevel, profile)
                .oneOf({C2Config::PROFILE_VP9_0,
                    C2Config::PROFILE_VP9_2,
                    C2Config::PROFILE_VP9_3}),
                C2F(mProfileLevel, level)
                .oneOf({C2Config::LEVEL_VP9_1, C2Config::LEVEL_VP9_1_1,
                    C2Config::LEVEL_VP9_2, C2Config::LEVEL_VP9_2_1,
                    C2Config::LEVEL_VP9_3, C2Config::LEVEL_VP9_3_1,
                    C2Config::LEVEL_VP9_4, C2Config::LEVEL_VP9_4_1,
                    C2Config::LEVEL_VP9_5, C2Config::LEVEL_VP9_5_1,
                    C2Config::LEVEL_VP9_6, C2Config::LEVEL_VP9_6_1})
            })
        .withSetter(ProfileLevelSetter)
        .build());
    }
}

void C2VdecComponent::IntfImpl::onAv1DeclareParam() {
    C2VendorCodec vendorCodec = C2VdecCodecConfig::getInstance().adaptorInputCodecToVendorCodec(mInputCodec);
    if (!C2VdecCodecConfig::getInstance().isCodecSupport8k(vendorCodec, mSecureMode)) {
        addParameter(
            DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(
                    0u, C2Config::PROFILE_AV1_0, C2Config::LEVEL_AV1_5_1))
            .withFields(
            {
                C2F(mProfileLevel, profile)
                .oneOf({C2Config::PROFILE_AV1_0,
                    C2Config::PROFILE_AV1_1,
                    C2Config::PROFILE_AV1_2}),
                C2F(mProfileLevel, level)
                .oneOf({C2Config::LEVEL_AV1_2, C2Config::LEVEL_AV1_2_1,
                    C2Config::LEVEL_AV1_2_2, C2Config::LEVEL_AV1_2_3,
                    C2Config::LEVEL_AV1_3, C2Config::LEVEL_AV1_3_1,
                    C2Config::LEVEL_AV1_3_2, C2Config::LEVEL_AV1_3_3,
                    C2Config::LEVEL_AV1_4, C2Config::LEVEL_AV1_4_1,
                    C2Config::LEVEL_AV1_4_2, C2Config::LEVEL_AV1_4_3,
                    C2Config::LEVEL_AV1_5, C2Config::LEVEL_AV1_5_1})
            })
        .withSetter(ProfileLevelSetter)
        .build());
    } else {
        addParameter(
            DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(
                    0u, C2Config::PROFILE_AV1_0, C2Config::LEVEL_AV1_6_1))
            .withFields(
            {
                C2F(mProfileLevel, profile)
                .oneOf({C2Config::PROFILE_AV1_0,
                    C2Config::PROFILE_AV1_1,
                    C2Config::PROFILE_AV1_2}),
                C2F(mProfileLevel, level)
                .oneOf({C2Config::LEVEL_AV1_2, C2Config::LEVEL_AV1_2_1,
                    C2Config::LEVEL_AV1_2_2, C2Config::LEVEL_AV1_2_3,
                    C2Config::LEVEL_AV1_3, C2Config::LEVEL_AV1_3_1,
                    C2Config::LEVEL_AV1_3_2, C2Config::LEVEL_AV1_3_3,
                    C2Config::LEVEL_AV1_4, C2Config::LEVEL_AV1_4_1,
                    C2Config::LEVEL_AV1_4_2, C2Config::LEVEL_AV1_4_3,
                    C2Config::LEVEL_AV1_5, C2Config::LEVEL_AV1_5_1,
                    C2Config::LEVEL_AV1_6, C2Config::LEVEL_AV1_6_1})
            })
        .withSetter(ProfileLevelSetter)
        .build());
    }
}

void C2VdecComponent::IntfImpl::onDvheDeclareParam() {
    addParameter(
        DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
        .withDefault(new C2StreamProfileLevelInfo::input(
                0u, C2Config::PROFILE_DV_HE_05, C2Config::LEVEL_DV_MAIN_UHD_60))
        .withFields(
        {
            C2F(mProfileLevel, profile)
            .oneOf({C2Config::PROFILE_DV_HE_04,
                C2Config::PROFILE_DV_HE_05,
                C2Config::PROFILE_DV_HE_08}),
            C2F(mProfileLevel, level)
            .oneOf({C2Config::LEVEL_DV_MAIN_HD_24,
                C2Config::LEVEL_DV_MAIN_HD_30,
                C2Config::LEVEL_DV_MAIN_FHD_24,
                C2Config::LEVEL_DV_MAIN_FHD_30,
                C2Config::LEVEL_DV_MAIN_FHD_60,
                C2Config::LEVEL_DV_MAIN_UHD_24,
                C2Config::LEVEL_DV_MAIN_UHD_30,
                C2Config::LEVEL_DV_MAIN_UHD_48,
                C2Config::LEVEL_DV_MAIN_UHD_60,
                C2Config::LEVEL_DV_HIGH_HD_24,
                C2Config::LEVEL_DV_HIGH_HD_30,
                C2Config::LEVEL_DV_HIGH_FHD_24,
                C2Config::LEVEL_DV_HIGH_FHD_30,
                C2Config::LEVEL_DV_HIGH_FHD_60,
                C2Config::LEVEL_DV_HIGH_UHD_24,
                C2Config::LEVEL_DV_HIGH_UHD_30,
                C2Config::LEVEL_DV_HIGH_UHD_48,
                C2Config::LEVEL_DV_HIGH_UHD_60})
        })
    .withSetter(ProfileLevelSetter)
    .build());
}

void C2VdecComponent::IntfImpl::onDvavDeclareParam() {
    addParameter(
        DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(
                    0u, C2Config::PROFILE_DV_AV_09, C2Config::LEVEL_DV_MAIN_UHD_60))
            .withFields(
            {
                C2F(mProfileLevel, profile)
                    .oneOf({C2Config::PROFILE_DV_AV_09}),
                C2F(mProfileLevel, level)
                    .oneOf({C2Config::LEVEL_DV_MAIN_HD_24,
                        C2Config::LEVEL_DV_MAIN_HD_30,
                        C2Config::LEVEL_DV_MAIN_FHD_24,
                        C2Config::LEVEL_DV_MAIN_FHD_30,
                        C2Config::LEVEL_DV_MAIN_FHD_60,
                        C2Config::LEVEL_DV_MAIN_UHD_24,
                        C2Config::LEVEL_DV_MAIN_UHD_30,
                        C2Config::LEVEL_DV_MAIN_UHD_48,
                        C2Config::LEVEL_DV_MAIN_UHD_60,
                        C2Config::LEVEL_DV_HIGH_HD_24,
                        C2Config::LEVEL_DV_HIGH_HD_30,
                        C2Config::LEVEL_DV_HIGH_FHD_24,
                        C2Config::LEVEL_DV_HIGH_FHD_30,
                        C2Config::LEVEL_DV_HIGH_FHD_60,
                        C2Config::LEVEL_DV_HIGH_UHD_24,
                        C2Config::LEVEL_DV_HIGH_UHD_30,
                        C2Config::LEVEL_DV_HIGH_UHD_48,
                        C2Config::LEVEL_DV_HIGH_UHD_60})
            })
    .withSetter(ProfileLevelSetter)
    .build());
}

void C2VdecComponent::IntfImpl::onDvav1DeclareParam() {
    addParameter(
        DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(
                    0u, C2Config::PROFILE_DV_AV1_10, C2Config::LEVEL_DV_MAIN_UHD_60))
            .withFields(
            {
                C2F(mProfileLevel, profile)
                .oneOf({C2Config::PROFILE_DV_AV1_10}),
                C2F(mProfileLevel, level)
                .oneOf({C2Config::LEVEL_DV_MAIN_HD_24,
                    C2Config::LEVEL_DV_MAIN_HD_30,
                    C2Config::LEVEL_DV_MAIN_FHD_24,
                    C2Config::LEVEL_DV_MAIN_FHD_30,
                    C2Config::LEVEL_DV_MAIN_FHD_60,
                    C2Config::LEVEL_DV_MAIN_UHD_24,
                    C2Config::LEVEL_DV_MAIN_UHD_30,
                    C2Config::LEVEL_DV_MAIN_UHD_48,
                    C2Config::LEVEL_DV_MAIN_UHD_60,
                    C2Config::LEVEL_DV_HIGH_HD_24,
                    C2Config::LEVEL_DV_HIGH_HD_30,
                    C2Config::LEVEL_DV_HIGH_FHD_24,
                    C2Config::LEVEL_DV_HIGH_FHD_30,
                    C2Config::LEVEL_DV_HIGH_FHD_60,
                    C2Config::LEVEL_DV_HIGH_UHD_24,
                    C2Config::LEVEL_DV_HIGH_UHD_30,
                    C2Config::LEVEL_DV_HIGH_UHD_48,
                    C2Config::LEVEL_DV_HIGH_UHD_60})
            })
    .withSetter(ProfileLevelSetter)
    .build());
}

void C2VdecComponent::IntfImpl::onMp2vDeclareParam() {
    addParameter(
        DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(
                    0u, C2Config::PROFILE_MP2V_HIGH, C2Config::LEVEL_MP2V_HIGH))
            .withFields(
            {
                C2F(mProfileLevel, profile)
                    .oneOf({C2Config::PROFILE_MP2V_SIMPLE,
                        C2Config::PROFILE_MP2V_MAIN,
                        C2Config::PROFILE_MP2V_SNR_SCALABLE,
                        C2Config::PROFILE_MP2V_SPATIALLY_SCALABLE,
                        C2Config::PROFILE_MP2V_HIGH,}),
                C2F(mProfileLevel, level)
                    .oneOf({C2Config::LEVEL_MP2V_LOW,
                        C2Config::LEVEL_MP2V_MAIN,
                        C2Config::LEVEL_MP2V_HIGH_1440,
                        C2Config::LEVEL_MP2V_HIGH,
                        C2Config::LEVEL_MP2V_HIGHP})
            })
    .withSetter(ProfileLevelSetter)
    .build());
}

void C2VdecComponent::IntfImpl::onMp4vDeclareParam() {
    addParameter(
        DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(
                    0u, C2Config::PROFILE_DV_HE_05, C2Config::LEVEL_DV_MAIN_UHD_60))
            .withFields(
            {
                C2F(mProfileLevel, profile)
                .oneOf({C2Config::PROFILE_MP4V_SIMPLE,
                    C2Config::PROFILE_MP4V_SIMPLE_SCALABLE,
                    C2Config::PROFILE_MP4V_MAIN,
                    C2Config::PROFILE_MP4V_NBIT,
                    C2Config::PROFILE_MP4V_ARTS}),
                C2F(mProfileLevel, level)
                .oneOf({C2Config::LEVEL_MP4V_0,
                    C2Config::LEVEL_MP4V_0B,
                    C2Config::LEVEL_MP4V_1,
                    C2Config::LEVEL_MP4V_2,
                    C2Config::LEVEL_MP4V_3,
                    C2Config::LEVEL_MP4V_3B,
                    C2Config::LEVEL_MP4V_4,
                    C2Config::LEVEL_MP4V_4A,
                    C2Config::LEVEL_MP4V_5})
            })
    .withSetter(ProfileLevelSetter)
    .build());
}

void C2VdecComponent::IntfImpl::onMjpgDeclareParam() {
    /*
    addParameter(
        DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(
                    0u, C2Config::PROFILE_DV_HE_05, C2Config::LEVEL_DV_MAIN_UHD_60))
            .withFields(
            {
                C2F(mProfileLevel, profile)
                    .oneOf({C2Config::PROFILE_MP4V_SIMPLE,
                        C2Config::PROFILE_MP4V_SIMPLE_SCALABLE,
                        C2Config::PROFILE_MP4V_MAIN,
                        C2Config::PROFILE_MP4V_NBIT,
                        C2Config::PROFILE_MP4V_ARTS}),
                C2F(mProfileLevel, level)
                    .oneOf({C2Config::LEVEL_MP4V_0,
                        C2Config::LEVEL_MP4V_0B,
                        C2Config::LEVEL_MP4V_1,
                        C2Config::LEVEL_MP4V_2,
                        C2Config::LEVEL_MP4V_3,
                        C2Config::LEVEL_MP4V_3B,
                        C2Config::LEVEL_MP4V_4,
                        C2Config::LEVEL_MP4V_4A,
                        C2Config::LEVEL_MP4V_5})
            })
    .withSetter(ProfileLevelSetter)
    .build());
    */
}

void C2VdecComponent::IntfImpl::onAvsDeclareParam() {
}

void C2VdecComponent::IntfImpl::onAvs2DeclareParam() {
}

void C2VdecComponent::IntfImpl::onAvs3DeclareParam() {
}

void C2VdecComponent::IntfImpl::onVc1DeclareParam() {
}

void C2VdecComponent::IntfImpl::onHdrDeclareParam(const std::shared_ptr<C2ReflectorHelper>& helper) {
    mHdrDynamicInfoInput = C2StreamHdrDynamicMetadataInfo::input::AllocShared(0);
    addParameter(
        DefineParam(mHdrDynamicInfoInput, C2_PARAMKEY_INPUT_HDR_DYNAMIC_INFO)
            .withDefault(mHdrDynamicInfoInput)
            .withFields({C2F(mHdrDynamicInfoInput, m.data).any(),})
    .withSetter(HdrDynamicInfoInputSetter)
    .build());

    mHdrDynamicInfoOutput = C2StreamHdrDynamicMetadataInfo::output::AllocShared(0);

    addParameter(
        DefineParam(mHdrDynamicInfoOutput, C2_PARAMKEY_OUTPUT_HDR_DYNAMIC_INFO)
            .withDefault(mHdrDynamicInfoOutput)
            .withFields({C2F(mHdrDynamicInfoOutput, m.data).any(),})
    .withSetter(HdrDynamicInfoOutputSetter)
    .build());

    // sample BT.2020 static info
    mHdrStaticInfo = std::make_shared<C2StreamHdrStaticInfo::output>();
    mHdrStaticInfo->mastering = {
        .red   = { .x = 0.0,  .y = 0.0 },
        .green = { .x = 0.0,  .y = 0.0 },
        .blue  = { .x = 0.0,  .y = 0.0 },
        .white = { .x = 0.0, .y = 0.0 },
        .maxLuminance = 0.0,
        .minLuminance = 0.0,
    };

    mHdrStaticInfo->maxCll = 0.0;
    mHdrStaticInfo->maxFall = 0.0;
    helper->addStructDescriptors<C2MasteringDisplayColorVolumeStruct, C2ColorXyStruct>();
    addParameter(
        DefineParam(mHdrStaticInfo, C2_PARAMKEY_HDR_STATIC_INFO)
        .withDefault(mHdrStaticInfo)
        .withFields({
            C2F(mHdrStaticInfo, mastering.red.x).inRange(0, 1),
        // TODO
        })
    .withSetter(HdrStaticInfoSetter)
    .build());
}

void C2VdecComponent::IntfImpl::onApiFeatureDeclareParam() {
    addParameter(
        DefineParam(mApiFeatures, C2_PARAMKEY_API_FEATURES)
        .withConstValue(new C2ApiFeaturesSetting(C2Config::api_feature_t(
            API_REFLECTION |
            API_VALUES |
            API_CURRENT_VALUES |
            API_DEPENDENCY |
            API_SAME_INPUT_BUFFER)))
    .build());
}

void C2VdecComponent::IntfImpl::onFrameRateDeclareParam() {
    addParameter(
        DefineParam(mFrameRateInfo,C2_PARAMKEY_FRAME_RATE)
            .withDefault(new C2StreamFrameRateInfo::input(0u, 30.0))
            .withFields({C2F(mFrameRateInfo,value).greaterThan(0.)})
    .withSetter(Setter<decltype(*mFrameRateInfo)>::StrictValueWithNoDeps)
    .build());
}

void C2VdecComponent::IntfImpl::onUnstablePtsDeclareParam() {
    addParameter(
        DefineParam(mUnstablePts,C2_PARAMKEY_UNSTABLE_PTS)
            .withDefault(new C2StreamUnstablePts::input(0))
            .withFields({C2F(mUnstablePts,enable).any()})
    .withSetter(StreamPtsUnstableSetter)
    .build());
}

void C2VdecComponent::IntfImpl::onOutputDelayDeclareParam() {
    int32_t propOutputDelay = property_get_int32(C2_PROPERTY_VDEC_OUT_DELAY,kDefaultOutputDelay);

    addParameter(
        DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
        .withDefault(new C2PortActualDelayTuning::output(propOutputDelay))
        .withFields({C2F(mActualOutputDelay, value).inRange(0, kMaxOutputDelay)})
    .withSetter(Setter<decltype(*mActualOutputDelay)>::StrictValueWithNoDeps)
    .build());
}

void C2VdecComponent::IntfImpl::onInputDelayDeclareParam() {
    int32_t inputDelayNum = 0;
    int32_t inputDelayMax = 0;
    if (mSecureMode) {
        inputDelayNum = property_get_int32(C2_PROPERTY_VDEC_INPUT_DELAY_NUM_SECURE, 4);
        if (inputDelayNum > kMaxInputDelaySecure) {
            CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d] exceed max secure input delay num %d %d",
                    __func__, __LINE__, inputDelayNum, kMaxInputDelaySecure);
            inputDelayNum = kMaxInputDelaySecure;
        }
        //secure buffer mode now we just support secure buffer mode
        addParameter(
        DefineParam(mSecureBufferMode, C2_PARAMKEY_SECURE_MODE)
            .withDefault(new C2SecureModeTuning(C2Config::SM_READ_PROTECTED))
            .withFields({C2F(mSecureBufferMode, value).inRange(C2Config::SM_UNPROTECTED, C2Config::SM_READ_PROTECTED)})
            .withSetter(Setter<decltype(*mSecureBufferMode)>::StrictValueWithNoDeps)
        .build());
        inputDelayMax = kMaxInputDelaySecure;
    } else {
        inputDelayNum = property_get_int32(C2_PROPERTY_VDEC_INPUT_DELAY_NUM, 4);
        if (inputDelayNum > kMaxInputDelay) {
            CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d] exceed max no-secure input delay num %d %d",
                    __func__, __LINE__, inputDelayNum, kMaxInputDelay);
            inputDelayNum = kMaxInputDelay;
        }
        inputDelayMax = kMaxInputDelay;
    }

    addParameter(
        DefineParam(mActualInputDelay, C2_PARAMKEY_INPUT_DELAY)
        .withDefault(new C2PortActualDelayTuning::input(inputDelayNum))
        .withFields({C2F(mActualInputDelay, value).inRange(0, inputDelayMax)})
        .withSetter(Setter<decltype(*mActualInputDelay)>::StrictValueWithNoDeps)
    .build());
}

void C2VdecComponent::IntfImpl::onReorderBufferDepthDeclareParam() {
    addParameter(
        DefineParam(mReorderBufferDepth, C2_PARAMKEY_OUTPUT_REORDER_DEPTH)
        .withConstValue(new C2PortReorderBufferDepthTuning(0u))
    .build());
}

void C2VdecComponent::IntfImpl::onPipelineDelayDeclareParam() {
    addParameter(
        DefineParam(mActualPipelineDelay, C2_PARAMKEY_PIPELINE_DELAY_REQUEST)
        .withConstValue(new C2ActualPipelineDelayTuning(0u))
    .build());
}

void C2VdecComponent::IntfImpl::onTunnelDeclareParam() {
    mTunnelModeOutput = C2PortTunneledModeTuning::output::AllocShared(
        1, C2PortTunneledModeTuning::Struct::SIDEBAND,
        C2PortTunneledModeTuning::Struct::REALTIME, -1);

    addParameter(
        DefineParam(mTunnelModeOutput, C2_PARAMKEY_TUNNELED_RENDER)
            .withDefault(mTunnelModeOutput)
            .withFields({
                C2F(mTunnelModeOutput, m.mode).any(),
                C2F(mTunnelModeOutput, m.syncType).any(),
            })
    .withSetter(TunnelModeOutSetter)
    .build());

    addParameter(
        DefineParam(mTunnelHandleOutput, C2_PARAMKEY_OUTPUT_TUNNEL_HANDLE)
            .withDefault(C2PortTunnelHandleTuning::output::AllocShared(AM_SIDEBAND_HANDLE_NUM_INT))
            .withFields({
                C2F(mTunnelHandleOutput, m.values[0]).any(),
                C2F(mTunnelHandleOutput, m.values).any()})
    .withSetter(TunnelHandleSetter)
    .build());

    addParameter(
        DefineParam(mTunnelSystemTimeOut, C2_PARAMKEY_OUTPUT_RENDER_TIME)
            .withDefault(new C2PortTunnelSystemTime::output(-1ll))
            .withFields({C2F(mTunnelSystemTimeOut, value).any()})
    .withSetter(TunnelSystemTimeSetter)
    .build());

    addParameter(
        DefineParam(mTunnelStartRender, C2_PARAMKEY_TUNNEL_START_RENDER)
            .withDefault(new C2StreamTunnelStartRender::output(0))
            .withFields({C2F(mTunnelStartRender, value).any()})
    .withSetter(C2_DEFAULT_UNSTRICT_SETTER(TunnelStartRender))
    .build());

    addParameter(
        DefineParam(mVendorNetflixVPeek, C2_PARAMKEY_VENDOR_NETFLIXVPEEK)
            .withDefault(new C2VendorNetflixVPeek::input(0))
            .withFields({{C2F(mVendorNetflixVPeek, vpeek).any()}})
    .withSetter(C2_DEFAULT_UNSTRICT_SETTER(VendorNetflixVPeek))
    .build());
}

void C2VdecComponent::IntfImpl::onTunerPassthroughDeclareParam() {
    addParameter(
        DefineParam(mVendorTunerHalParam, C2_PARAMKEY_VENDOR_TunerHal)
        .withDefault(new C2VendorTunerHalParam::input(0, 0))
        .withFields({
            C2F(mVendorTunerHalParam, videoFilterId).any(),
            C2F(mVendorTunerHalParam, hwAVSyncId).any()})
    .withSetter(C2_DEFAULT_UNSTRICT_SETTER(VendorTunerHalParam))
    .build());

    addParameter(
        DefineParam(mVendorTunerPassthroughTrickMode, C2_PARAMKEY_VENDOR_TunerPassthroughTrickMode)
        .withDefault(new C2VendorTunerPassthroughTrickMode::input(0, 1, 0))
        .withFields({
            C2F(mVendorTunerPassthroughTrickMode, trickMode).any(),
            C2F(mVendorTunerPassthroughTrickMode, trickSpeed).any(),
            C2F(mVendorTunerPassthroughTrickMode, frameAdvance).any()})
    .withSetter(C2_DEFAULT_UNSTRICT_SETTER(VendorTunerPassthroughTrickMode))
    .build());

    addParameter(
        DefineParam(mVendorTunerPassthroughWorkMode, C2_PARAMKEY_VENDOR_TunerPassthroughWorkMode)
        .withDefault(new C2VendorTunerPassthroughWorkMode::input(0))
        .withFields({
            C2F(mVendorTunerPassthroughWorkMode, workMode).any()})
    .withSetter(C2_DEFAULT_UNSTRICT_SETTER(VendorTunerPassthroughWorkMode))
    .build());

    addParameter(
        DefineParam(mVendorTunerPassthroughEventMask, C2_PARAMKEY_VENDOR_TunerPassthroughEventMask)
        .withDefault(new C2VendorTunerPassthroughEventMask::input(0))
        .withFields({
            C2F(mVendorTunerPassthroughEventMask, eventMask).any()})
    .withSetter(C2_DEFAULT_UNSTRICT_SETTER(VendorTunerPassthroughEventMask))
    .build());
}

void C2VdecComponent::IntfImpl::onGameModeLatencyDeclareParam() {
        addParameter(
                DefineParam(mVendorGameModeLatency, C2_PARAMKEY_VENDOR_GAME_MODE_LATENCY)
                .withDefault(new C2VendorGameModeLatency::input(0))
                .withFields({C2F(mVendorGameModeLatency, enable).any()})
        .withSetter(C2_DEFAULT_UNSTRICT_SETTER(VendorGameModeLatency))
        .build());
}


void C2VdecComponent::IntfImpl::onBufferSizeDeclareParam(const char* mine) {
    // Get supported profiles from Vdec.
    // TODO: re-think the suitable method of getting supported profiles for both pure Android and
    //       ARC++.
    media::VideoDecodeAccelerator::SupportedProfiles supportedProfiles;
    supportedProfiles = VideoDecWraper::AmVideoDec_getSupportedProfiles((uint32_t)mInputCodec);
    if (supportedProfiles.empty()) {
        CODEC2_LOG(CODEC2_LOG_ERR, "No supported profile from input codec: %d", mInputCodec);
        mInitStatus = C2_BAD_VALUE;
        return;
    }

    mCodecProfile = supportedProfiles[0].profile;

    auto minSize = supportedProfiles[0].min_resolution;
    auto maxSize = supportedProfiles[0].max_resolution;
    addParameter(
        DefineParam(mKind, C2_PARAMKEY_COMPONENT_KIND)
        .withConstValue(
        new C2ComponentKindSetting(C2Component::KIND_DECODER))
    .build());

    addParameter(
        DefineParam(mInputFormat, C2_PARAMKEY_INPUT_STREAM_BUFFER_TYPE)
        .withConstValue(
        new C2StreamBufferTypeSetting::input(0u, C2BufferData::LINEAR))
    .build());

    addParameter(
        DefineParam(mOutputFormat, C2_PARAMKEY_OUTPUT_STREAM_BUFFER_TYPE)
        .withConstValue(
        new C2StreamBufferTypeSetting::output(0u, C2BufferData::GRAPHIC))
    .build());

    addParameter(
        DefineParam(mInputMediaType, C2_PARAMKEY_INPUT_MEDIA_TYPE)
        .withConstValue(AllocSharedString<C2PortMediaTypeSetting::input>(mine))
    .build());

    addParameter(
        DefineParam(mOutputMediaType, C2_PARAMKEY_OUTPUT_MEDIA_TYPE)
        .withConstValue(AllocSharedString<C2PortMediaTypeSetting::output>(
        MEDIA_MIMETYPE_VIDEO_RAW))
    .build());

    addParameter(
        DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
        .withDefault(new C2StreamPictureSizeInfo::output(0u, 176, 144))
        .withFields({
            C2F(mSize, width).inRange(minSize.width(), maxSize.width(), 16),
            C2F(mSize, height).inRange(minSize.height(), maxSize.height(), 16),
        })
    .withSetter(SizeSetter)
    .build());

    // App may set a smaller value for maximum of input buffer size than actually required
    // by mistake. C2VdecComponent overrides it if the value specified by app is smaller than
    // the calculated value in MaxSizeCalculator().
    // This value is the default maximum of linear buffer size (kLinearBufferSize) in
    // CCodecBufferChannel.cpp.
    constexpr static size_t kLinearBufferSize = 1 * 1024 * 1024;
    static size_t kLinearPaddingBufferSize = 0;

    if (mSecureMode)
        kLinearPaddingBufferSize = 0;
    else
        kLinearPaddingBufferSize = 262144; // 256KB exoplayer 264/265/vp9

    struct LocalCalculator {
        static C2R MaxSizeCalculator(bool mayBlock, C2P<C2StreamMaxBufferSizeInfo::input>& me,
                                        const C2P<C2StreamPictureSizeInfo::output>& size) {
            (void)mayBlock;
            size_t maxInputSize = property_get_int32(C2_PROPERTY_VDEC_INPUT_MAX_SIZE, 6291456);
            size_t paddingSize = property_get_int32(C2_PROPERTY_VDEC_INPUT_MAX_PADDINGSIZE, kLinearPaddingBufferSize);
            size_t defaultSize = me.get().value;
            if (defaultSize > kMaxInputBufferSize) {
               CODEC2_LOG(CODEC2_LOG_INFO,"The current input buffer size %zu is too large, limit its number", (size_t)me.set().value);
               return C2R::Ok();
            }

            if (defaultSize > 0) {
                if (paddingSize == 0 && defaultSize < 2 * kLinearBufferSize)
                    defaultSize = 2 * kLinearBufferSize;
                else
                    defaultSize += paddingSize;
            } else {
                defaultSize = kLinearBufferSize;
            }

            if (defaultSize > maxInputSize) {
                me.set().value = maxInputSize;
                CODEC2_LOG(CODEC2_LOG_INFO,"Force setting %u to max is %zu", me.get().value, maxInputSize);
            } else {
                me.set().value = defaultSize;
            }

            //app may set too small
            if (((size.v.width * size.v.height) > (1920 * 1088))
                && (me.set().value < (4 * kLinearBufferSize))) {
                me.set().value = 4 * kLinearBufferSize;
            }

            return C2R::Ok();
        }
    };

    addParameter(
        DefineParam(mMaxInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
        .withDefault(new C2StreamMaxBufferSizeInfo::input(0u, kLinearBufferSize))
        .withFields({
            C2F(mMaxInputSize, value).any(),
        })
    .calculatedAs(LocalCalculator::MaxSizeCalculator, mSize)
    .build());
}

void C2VdecComponent::IntfImpl::onBufferPoolDeclareParam() {
    C2Allocator::id_t inputAllocators[] = { C2PlatformAllocatorStore::DMABUFHEAP};

    C2Allocator::id_t outputAllocators[] = {C2PlatformAllocatorStore::GRALLOC};

    C2Allocator::id_t surfaceAllocator = C2PlatformAllocatorStore::BUFFERQUEUE;

    addParameter(
        DefineParam(mInputAllocatorIds, C2_PARAMKEY_INPUT_ALLOCATORS)
        .withConstValue(C2PortAllocatorsTuning::input::AllocShared(inputAllocators))
    .build());

    addParameter(
        DefineParam(mOutputAllocatorIds, C2_PARAMKEY_OUTPUT_ALLOCATORS)
        .withConstValue(C2PortAllocatorsTuning::output::AllocShared(outputAllocators))
    .build());

    addParameter(
        DefineParam(mOutputSurfaceAllocatorId, C2_PARAMKEY_OUTPUT_SURFACE_ALLOCATOR)
        .withDefault(new C2PortSurfaceAllocatorTuning::output(surfaceAllocator))
        .withFields({C2F(mOutputSurfaceAllocatorId, value).any()})
        .withSetter(OutSurfaceAllocatorIdSetter)
    .build());

    C2BlockPool::local_id_t outputBlockPools[] = {kDefaultOutputBlockPool};

    addParameter(
        DefineParam(mOutputBlockPoolIds, C2_PARAMKEY_OUTPUT_BLOCK_POOLS)
            .withDefault(C2PortBlockPoolsTuning::output::AllocShared(outputBlockPools))
            .withFields({C2F(mOutputBlockPoolIds, m.values[0]).any(),
                        C2F(mOutputBlockPoolIds, m.values).inRange(0, 1)})
            .withSetter(Setter<C2PortBlockPoolsTuning::output>::NonStrictValuesWithNoDeps)
    .build());
}

void C2VdecComponent::IntfImpl::onColorAspectsDeclareParam() {
    addParameter(
        DefineParam(mDefaultColorAspects, C2_PARAMKEY_DEFAULT_COLOR_ASPECTS)
        .withDefault(new C2StreamColorAspectsTuning::output(
            0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
            C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
            .withFields(
            {C2F(mDefaultColorAspects, range)
                .inRange(C2Color::RANGE_UNSPECIFIED, C2Color::RANGE_OTHER),
            C2F(mDefaultColorAspects, primaries)
                .inRange(C2Color::PRIMARIES_UNSPECIFIED,
                    C2Color::PRIMARIES_OTHER),
            C2F(mDefaultColorAspects, transfer)
                .inRange(C2Color::TRANSFER_UNSPECIFIED,
                    C2Color::TRANSFER_OTHER),
            C2F(mDefaultColorAspects, matrix)
                    .inRange(C2Color::MATRIX_UNSPECIFIED, C2Color::MATRIX_OTHER)})
    .withSetter(DefaultColorAspectsSetter)
    .build());

    addParameter(
        DefineParam(mCodedColorAspects, C2_PARAMKEY_VUI_COLOR_ASPECTS)
        .withDefault(new C2StreamColorAspectsInfo::input(
            0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
            C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
        .withFields(
            {C2F(mCodedColorAspects, range)
                .inRange(C2Color::RANGE_UNSPECIFIED, C2Color::RANGE_OTHER),
            C2F(mCodedColorAspects, primaries)
                .inRange(C2Color::PRIMARIES_UNSPECIFIED,
                    C2Color::PRIMARIES_OTHER),
            C2F(mCodedColorAspects, transfer)
                .inRange(C2Color::TRANSFER_UNSPECIFIED,
                    C2Color::TRANSFER_OTHER),
            C2F(mCodedColorAspects, matrix)
                .inRange(C2Color::MATRIX_UNSPECIFIED, C2Color::MATRIX_OTHER)})
    .withSetter(DefaultColorAspectsSetter)
    .build());

    addParameter(
        DefineParam(mColorAspects, C2_PARAMKEY_COLOR_ASPECTS)
        .withDefault(new C2StreamColorAspectsInfo::output(
            0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
            C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
        .withFields(
            {C2F(mColorAspects, range)
                .inRange(C2Color::RANGE_UNSPECIFIED, C2Color::RANGE_OTHER),
            C2F(mColorAspects, primaries)
                .inRange(C2Color::PRIMARIES_UNSPECIFIED,
                C2Color::PRIMARIES_OTHER),
            C2F(mColorAspects, transfer)
                .inRange(C2Color::TRANSFER_UNSPECIFIED,
                C2Color::TRANSFER_OTHER),
            C2F(mColorAspects, matrix)
                .inRange(C2Color::MATRIX_UNSPECIFIED, C2Color::MATRIX_OTHER)})
    .withSetter(MergedColorAspectsSetter, mDefaultColorAspects, mCodedColorAspects)
    .build());
}

void C2VdecComponent::IntfImpl::onLowLatencyDeclareParam() {
    addParameter(
        DefineParam(mLowLatencyMode, C2_PARAMKEY_LOW_LATENCY_MODE)
            .withDefault(new C2GlobalLowLatencyModeTuning(false))
            .withFields({C2F(mLowLatencyMode, value).oneOf({true, false})})
            .withSetter(LowLatencyModeSetter)
    .build());
}

void C2VdecComponent::IntfImpl::onPixelFormatDeclareParam() {
    bool support_soft_10bit = property_get_bool(C2_PROPERTY_VDEC_SUPPORT_10BIT, true);
    bool support_hardware_10bit = property_get_bool(PROPERTY_PLATFORM_SUPPORT_HARDWARE_P010, false);
    if (support_soft_10bit || support_hardware_10bit) {
        std::vector<uint32_t> pixelFormats = { HAL_PIXEL_FORMAT_YCBCR_420_888 };
        if (isHalPixelFormatSupported((AHardwareBuffer_Format)HAL_PIXEL_FORMAT_YCBCR_P010)) {
           pixelFormats.push_back(HAL_PIXEL_FORMAT_YCBCR_P010);
           CODEC2_LOG(CODEC2_LOG_INFO, "support 10bit");
        } else {
           CODEC2_LOG(CODEC2_LOG_INFO, "not support 10bit");
        }

        pixelFormats.push_back(HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED);
        addParameter(
            DefineParam(mPixelFormatInfo, C2_PARAMKEY_PIXEL_FORMAT)
                .withDefault(new C2StreamPixelFormatInfo::output(
                            0u,  HAL_PIXEL_FORMAT_YCBCR_420_888))
                .withFields({C2F(mPixelFormatInfo, value).oneOf(pixelFormats)})
        .withSetter((Setter<decltype(*mPixelFormatInfo)>::StrictValueWithNoDeps))
        .build());
    } else {
        CODEC2_LOG(CODEC2_LOG_INFO, "not set prop not support 10bit");
    }
}

void C2VdecComponent::IntfImpl::onVendorExtendParam() {
    addParameter(
        DefineParam(mVdecWorkMode, C2_PARAMKEY_VENDOR_VDEC_WORK_MODE)
            .withDefault(new C2VdecWorkMode::input(0))
            .withFields({C2F(mVdecWorkMode, value).any()})
    .withSetter(Setter<decltype(*mVdecWorkMode)>::StrictValueWithNoDeps)
    .build());

    addParameter(
        DefineParam(mDataSourceType, C2_PARAMKEY_VENDOR_DATASOURCE_TYPE)
            .withDefault(new C2DataSourceType::input(0))
            .withFields({C2F(mDataSourceType, value).any()})
    .withSetter(Setter<decltype(*mDataSourceType)>::StrictValueWithNoDeps)
    .build());

    addParameter(
        DefineParam(mStreamModeInputDelay, C2_PARAMKEY_VENDOR_STREAMMODE_INPUT_DELAY)
            .withDefault(new C2StreamModeInputDelay::input(0))
            .withFields({C2F(mStreamModeInputDelay, value).inRange(0, kMaxInputDelayStreamMode)})
    .withSetter(Setter<decltype(*mStreamModeInputDelay)>::StrictValueWithNoDeps)
    .build());

    addParameter(
        DefineParam(mStreamModePipeLineDelay, C2_PARAMKEY_VENDOR_STREAMMODE_PIPELINE_DELAY)
            .withDefault(new C2StreamModePipeLineDelay::input(0))
            .withFields({C2F(mStreamModePipeLineDelay, value).inRange(0, kMaxPipeLineDelayStreamMode)})
    .withSetter(Setter<decltype(*mStreamModePipeLineDelay)>::StrictValueWithNoDeps)
    .build());

    addParameter(
    DefineParam(mStreamModeHwAvSyncId, C2_PARAMKEY_VENDOR_STREAMMODE_HWAVSYNCID)
            .withDefault(new C2StreamModeHwAvSyncId::input(0))
            .withFields({C2F(mStreamModeHwAvSyncId, value).any()})
            .withSetter(Setter<decltype(*mStreamModeHwAvSyncId)>::StrictValueWithNoDeps)
    .build());

    addParameter(
    DefineParam(mPlayerId,C2_PARAMKEY_PLAYER_ID)
           .withDefault(new C2VendorPlayerId::input(0))
           .withFields({C2F(mPlayerId,value).any()})
           .withSetter(Setter<decltype(*mPlayerId)>::StrictValueWithNoDeps)
    .build());

    addParameter(
        DefineParam(mErrorPolicy, C2_PARAMKEY_VENDOR_ERROR_POLICY)
            .withDefault(new C2ErrorPolicy::input(1))
            .withFields({C2F(mErrorPolicy, value).any()})
    .withSetter(Setter<decltype(*mErrorPolicy)>::StrictValueWithNoDeps)
    .build());
}

void C2VdecComponent::IntfImpl::onAvc4kMMUEnable() {
    addParameter(
        DefineParam(mAvc4kMMUMode, C2_PARAMKEY_VENDOR_AVC_4K_MMU)
        .withDefault(new C2Avc4kMMU::input(0))
        .withFields({C2F(mAvc4kMMUMode, value).any()})
    .withSetter(Setter<decltype(*mAvc4kMMUMode)>::StrictValueWithNoDeps)
    .build());
}

// config param
void C2VdecComponent::IntfImpl::onTunneledModeTuningConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]tunnel mode config",
            mComponent->mSessionID, mComponent->mDecoderID);
    mComponent->onConfigureTunnelMode();
    // change to bufferpool
    mOutputSurfaceAllocatorId->value = C2PlatformAllocatorStore::GRALLOC;
    mActualOutputDelay->value = kDefaultOutputDelayTunnel;
}


void C2VdecComponent::IntfImpl::onVendorTunerHalConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]passthrough mode config",
            mComponent->mSessionID, mComponent->mDecoderID);
    mComponent->onConfigureTunerPassthroughMode();
}


void C2VdecComponent::IntfImpl::onTunerPassthroughTrickModeConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]tuner passthrough trick mode config",
            mComponent->mSessionID, mComponent->mDecoderID);
    mComponent->onConfigureTunerPassthroughTrickMode();
}

void C2VdecComponent::IntfImpl::onTunerPassthroughWorkModeConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]tuner passthrough work mode config",
            mComponent->mSessionID, mComponent->mDecoderID);
    mComponent->onConfigureTunerPassthroughWorkMode();
}

void C2VdecComponent::IntfImpl::onTunerPassthroughEventMaskConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]tuner passthrough event mask config",
            mComponent->mSessionID, mComponent->mDecoderID);
    mComponent->onConfigureTunerPassthroughEventMask();
}

void C2VdecComponent::IntfImpl::onVdecWorkModeConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]config vdec work mode:%d",
            mComponent->mSessionID, mComponent->mDecoderID, mVdecWorkMode->value);
}

void C2VdecComponent::IntfImpl::onDataSourceTypeConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]config datasource type:%d", mComponent->mSessionID, mComponent->mDecoderID, mDataSourceType->value);
}

void C2VdecComponent::IntfImpl::onStreamModeInputDelayConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]config stream mode input delay :%d",
        mComponent->mSessionID, mComponent->mDecoderID, mStreamModeInputDelay->value);
    mActualInputDelay->value = mStreamModeInputDelay->value;
}

void C2VdecComponent::IntfImpl::onStreamModePipeLineDelayConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]config stream mode pipeline delay :%d",
        mComponent->mSessionID, mComponent->mDecoderID, mStreamModePipeLineDelay->value);
    mActualPipelineDelay->value = mStreamModePipeLineDelay->value;
}

void C2VdecComponent::IntfImpl::onStreamTunnelStartRenderConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]config tunnel startRender",
        mComponent->mSessionID, mComponent->mDecoderID);
    mComponent->onAndroidVideoPeek();
}

void C2VdecComponent::IntfImpl::onStreamHdr10PlusInfoConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]config HDR10PlusInfo",
        mComponent->mSessionID, mComponent->mDecoderID);
}

void C2VdecComponent::IntfImpl::onStreamMaxBufferSizeInfoConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]config C2StreamMaxBufferSizeInfo",
            mComponent->mSessionID, mComponent->mDecoderID);

    bool isLowMemDevice = !property_get_bool(PROPERTY_PLATFORM_SUPPORT_4K, true);
    size_t maxInputSize = property_get_int32(C2_PROPERTY_VDEC_INPUT_MAX_SIZE, kMaxInputBufferSize);

    if (isLowMemDevice && (mMaxInputSize->value > maxInputSize)) {
        CODEC2_LOG(CODEC2_LOG_INFO, "set max input size to %zu", maxInputSize);
        mActualInputDelay->value = 0;
        mMaxInputSize->value = maxInputSize;
    }
}

c2_status_t C2VdecComponent::IntfImpl::onStreamPictureSizeConfigParam(
        std::vector<std::unique_ptr<C2SettingResult>>* const failures, C2Param* const param) {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]config picture w:%d h:%d",
                            C2VdecComponent::mInstanceID, mComponent->mSessionID,
                            mSize->width, mSize->height);
    c2_status_t ret = C2_OK;
    int32_t bufferSize = mSize->width * mSize->height;

    C2VendorCodec vendorCodec = C2VdecCodecConfig::getInstance().adaptorInputCodecToVendorCodec(mInputCodec);
    ret = C2VdecCodecConfig::getInstance().isCodecSupportResolutionRatio(mInputCodec, mSecureMode, bufferSize);

    if (ret != C2_OK) {
        CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d] not support 8K for non-8K platform or 4K for non-4K platform, config failed",
                C2VdecComponent::mInstanceID, mComponent->mSessionID);
        std::unique_ptr<C2SettingResult> result = std::unique_ptr<C2SettingResult>(new C2SettingResult {
            .field = C2ParamFieldValues(C2ParamField(param)),
            .failure = C2SettingResult::Failure::BAD_VALUE,
        });
        failures->emplace_back(std::move(result));
    }

    /*Check if use max resolution*/
    bool isMaxRes = C2VdecCodecConfig::getInstance().isMaxResolutionFromXml(vendorCodec, mSecureMode, mSize->width, mSize->height);

    if (isMaxRes && !mComponent->mIsMaxResolution) {
        C2VdecComponent::sConcurrentMaxResolutionInstance.fetch_add(1, std::memory_order_relaxed);
        mComponent->mIsMaxResolution = true;
        CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d] use max res, ins:%d",
            C2VdecComponent::mInstanceID, mComponent->mSessionID, C2VdecComponent::sConcurrentMaxResolutionInstance.load());
    }

    return ret;
}

bool C2VdecComponent::IntfImpl::onStreamFrameRateConfigParam(
        std::vector<std::unique_ptr<C2SettingResult>>* const failures, C2Param* const param) {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]config frame rate:%f",
                            C2VdecComponent::mInstanceID, mComponent->mSessionID, mFrameRateInfo->value);

    C2VendorCodec vendorCodec = C2VdecCodecConfig::getInstance().adaptorInputCodecToVendorCodec(mInputCodec);
    bool ret = C2VdecCodecConfig::getInstance().isCodecSupportFrameRate(vendorCodec, mSecureMode, mSize->width, mSize->height, mFrameRateInfo->value);
    if (ret != true) {
        std::unique_ptr<C2SettingResult> result = std::unique_ptr<C2SettingResult>(new C2SettingResult {
            .field = C2ParamFieldValues(C2ParamField(param)),
            .failure = C2SettingResult::Failure::BAD_VALUE,
        });
        failures->emplace_back(std::move(result));
    }

    return ret;
}

void C2VdecComponent::IntfImpl::onNetflixVPeekConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d]config netflix vpeek:%d",
        C2VdecComponent::mInstanceID, mComponent->mSessionID, mVendorNetflixVPeek->vpeek);
}

void C2VdecComponent::IntfImpl::onErrorPolicyConfigParam() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d] config error policy :%d",
                        mComponent->mSessionID, mComponent->mDecoderID, mErrorPolicy->value);
}

}
