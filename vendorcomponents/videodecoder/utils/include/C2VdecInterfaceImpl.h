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

#ifndef _C2_Vdec_INTERFACE_IMPL_H_
#define _C2_Vdec_INTERFACE_IMPL_H_

#include <C2VdecComponent.h>
#include "Codec2CommonUtils.h"
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>

namespace android {

class C2VdecComponent::IntfImpl : public C2InterfaceHelper {
public:
    IntfImpl(C2String name, const std::shared_ptr<C2ReflectorHelper>& helper);

    c2_status_t config(
            const std::vector<C2Param*> &params, c2_blocking_t mayBlock,
            std::vector<std::unique_ptr<C2SettingResult>>* const failures,
            bool updateParams = true,
            std::vector<std::shared_ptr<C2Param>> *changes = nullptr);

    // interfaces for C2VdecComponent
    c2_status_t status() const {return mInitStatus;}
    media::VideoCodecProfile getCodecProfile() {return mCodecProfile;}
    C2BlockPool::local_id_t getBlockPoolId() const {return mOutputBlockPoolIds->m.values[0];}
    InputCodec getInputCodec() {return mInputCodec;}
    float getInputFrameRate() {return  mFrameRateInfo->value;}
    uint32_t getPixelFormatInfoValue() {return mPixelFormatInfo->value;}
    void getHdr10PlusBuf(uint8_t** pbuf, uint32_t* plen);

    // for DolbyVision Only
    void updateInputCodec(InputCodec videotype);
    void updateCodecProfile(media::VideoCodecProfile profile) {mCodecProfile = profile;}
    void setComponent(C2VdecComponent* comp) {mComponent = comp;}

    std::shared_ptr<C2StreamColorAspectsInfo::output> getColorAspects() {return mColorAspects;}
    std::shared_ptr<C2StreamColorAspectsTuning::output> getDefaultColorAspects() {return mDefaultColorAspects;}
    std::shared_ptr<C2StreamHdrDynamicMetadataInfo::input> getHdr10PlusInfo() {return mHdrDynamicInfoInput;}

    void updateHdr10PlusInfoToWork(C2Work& work);

private:
    // Configurable parameter setters.
    static C2R ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::input>& info);
    static C2R SizeSetter(bool mayBlock, C2P<C2StreamPictureSizeInfo::output>& videoSize);

    template <typename T>
    static C2R DefaultColorAspectsSetter(bool mayBlock, C2P<T>& def);

    static C2R MergedColorAspectsSetter(bool mayBlock,
                                        C2P<C2StreamColorAspectsInfo::output>& merged,
                                        const C2P<C2StreamColorAspectsTuning::output>& def,
                                        const C2P<C2StreamColorAspectsInfo::input>& coded);
    static C2R Hdr10PlusInfoInputSetter(bool mayBlock, C2P<C2StreamHdr10PlusInfo::input> &me);
    static C2R Hdr10PlusInfoOutputSetter(bool mayBlock, C2P<C2StreamHdr10PlusInfo::output> &me);
    static C2R HdrStaticInfoSetter(bool mayBlock, C2P<C2StreamHdrStaticInfo::output> &me);
    static C2R LowLatencyModeSetter(bool mayBlock, C2P<C2GlobalLowLatencyModeTuning> &me);
    static C2R OutSurfaceAllocatorIdSetter(bool mayBlock, C2P<C2PortSurfaceAllocatorTuning::output> &me);
    static C2R TunnelModeOutSetter(bool mayBlock, C2P<C2PortTunneledModeTuning::output> &me);
    static C2R TunnelHandleSetter(bool mayBlock, C2P<C2PortTunnelHandleTuning::output> &me);
    static C2R TunnelSystemTimeSetter(bool mayBlock, C2P<C2PortTunnelSystemTime::output> &me);
    static C2R StreamPtsUnstableSetter(bool mayBlock, C2P<C2StreamUnstablePts::input> &me);

    //declare some unstrict Setter
    DECLARE_C2_DEFAULT_UNSTRICT_SETTER(C2VendorTunerHalParam::input, VendorTunerHalParam);
    DECLARE_C2_DEFAULT_UNSTRICT_SETTER(C2StreamTunnelStartRender::output, TunnelStartRender);
    DECLARE_C2_DEFAULT_UNSTRICT_SETTER(C2VendorTunerPassthroughTrickMode::input, VendorTunerPassthroughTrickMode);
    DECLARE_C2_DEFAULT_UNSTRICT_SETTER(C2VendorTunerPassthroughWorkMode::input, VendorTunerPassthroughWorkMode);
    DECLARE_C2_DEFAULT_UNSTRICT_SETTER(C2VendorTunerPassthroughInstanceNo::input, VendorTunerPassthroughInstanceNo);
    DECLARE_C2_DEFAULT_UNSTRICT_SETTER(C2VendorNetflixVPeek::input, VendorNetflixVPeek);
    DECLARE_C2_DEFAULT_UNSTRICT_SETTER(C2StreamHdrDynamicMetadataInfo::input, HdrDynamicInfoInput);
    DECLARE_C2_DEFAULT_UNSTRICT_SETTER(C2StreamHdrDynamicMetadataInfo::output, HdrDynamicInfoOutput);
    DECLARE_C2_DEFAULT_UNSTRICT_SETTER(C2VendorTunerPassthroughEventMask::input, VendorTunerPassthroughEventMask);
    DECLARE_C2_DEFAULT_UNSTRICT_SETTER(C2VendorGameModeLatency::input, VendorGameModeLatency);

    std::shared_ptr<C2ApiLevelSetting> mApiLevel;
    std::shared_ptr<C2ApiFeaturesSetting> mApiFeatures;

    // The kind of the component; should be C2Component::KIND_ENCODER.
    std::shared_ptr<C2ComponentKindSetting> mKind;
    // The input format kind; should be C2FormatCompressed.
    std::shared_ptr<C2StreamBufferTypeSetting::input> mInputFormat;
    // The output format kind; should be C2FormatVideo.
    std::shared_ptr<C2StreamBufferTypeSetting::output> mOutputFormat;
    // The MIME type of input port.
    std::shared_ptr<C2PortMediaTypeSetting::input> mInputMediaType;
    // The MIME type of output port; should be MEDIA_MIMETYPE_VIDEO_RAW.
    std::shared_ptr<C2PortMediaTypeSetting::output> mOutputMediaType;
    // The input codec profile and level. For now configuring this parameter is useless since
    // the component always uses fixed codec profile to initialize accelerator. It is only used
    // for the client to query supported profile and level values.
    // TODO: use configured profile/level to initialize accelerator.
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
    // Decoded video size for output.
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    // Maximum size of one input buffer.
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
    // The suggested usage of input buffer allocator ID.
    std::shared_ptr<C2PortAllocatorsTuning::input> mInputAllocatorIds;
    // The suggested usage of output buffer allocator ID.
    std::shared_ptr<C2PortAllocatorsTuning::output> mOutputAllocatorIds;
    // The suggested usage of output buffer allocator ID with surface.
    std::shared_ptr<C2PortSurfaceAllocatorTuning::output> mOutputSurfaceAllocatorId;
    // Component uses this ID to fetch corresponding output block pool from platform.
    std::shared_ptr<C2PortBlockPoolsTuning::output> mOutputBlockPoolIds;
    // The color aspects parsed from input bitstream. This parameter should be configured by
    // Component while decoding.
    std::shared_ptr<C2StreamColorAspectsInfo::input> mCodedColorAspects;
    // The default color aspects specified by requested output format. This parameter should be
    // configured by client.
    std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
    // The combined color aspects by |mCodedColorAspects| and |mDefaultColorAspects|, and the
    // former has higher priority. This parameter is used for component to provide color aspects
    // as C2Info in decoded output buffers.
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
    //hdr
    std::shared_ptr<C2StreamHdrStaticInfo::output> mHdrStaticInfo;
    std::shared_ptr<C2StreamHdrDynamicMetadataInfo::input> mHdrDynamicInfoInput;
    std::shared_ptr<C2StreamHdrDynamicMetadataInfo::output> mHdrDynamicInfoOutput;
    std::shared_ptr<C2PortActualDelayTuning::input> mActualInputDelay;

    //frame rate
    std::shared_ptr<C2StreamFrameRateInfo::input> mFrameRateInfo;
    //unstable pts
    std::shared_ptr<C2StreamUnstablePts::input> mUnstablePts;
    //player id
    std::shared_ptr<C2VendorPlayerId::input> mPlayerId;
    //stream mode
    std::shared_ptr<C2VdecWorkMode::input> mVdecWorkMode;
    //dmx source
    std::shared_ptr<C2DataSourceType::input> mDataSourceType;
    //stream mode input delay value
    std::shared_ptr<C2StreamModeInputDelay::input> mStreamModeInputDelay;
    //stream mode pipeline delay value
    std::shared_ptr<C2StreamModePipeLineDelay::input> mStreamModePipeLineDelay;
    //stream mode hwavsyncid value
    std::shared_ptr<C2StreamModeHwAvSyncId::input> mStreamModeHwAvSyncId;

    std::shared_ptr<C2PortActualDelayTuning::output> mActualOutputDelay;
    std::shared_ptr<C2ActualPipelineDelayTuning> mActualPipelineDelay;
    std::shared_ptr<C2PortReorderBufferDepthTuning> mReorderBufferDepth;

    //tunnel mode
    std::shared_ptr<C2PortTunneledModeTuning::output> mTunnelModeOutput;
    std::shared_ptr<C2PortTunnelHandleTuning::output> mTunnelHandleOutput;
    std::shared_ptr<C2PortTunnelSystemTime::output> mTunnelSystemTimeOut;
    std::shared_ptr<C2VendorTunerHalParam::input> mVendorTunerHalParam;
    std::shared_ptr<C2StreamTunnelStartRender::output> mTunnelStartRender;
    std::shared_ptr<C2VendorTunerPassthroughTrickMode::input> mVendorTunerPassthroughTrickMode;
    std::shared_ptr<C2VendorTunerPassthroughWorkMode::input> mVendorTunerPassthroughWorkMode;
    std::shared_ptr<C2VendorTunerPassthroughInstanceNo::input> mVendorTunerPassthroughInstanceNo;
    std::shared_ptr<C2VendorNetflixVPeek::input> mVendorNetflixVPeek;
    std::shared_ptr<C2VendorTunerPassthroughEventMask::input> mVendorTunerPassthroughEventMask;

    std::shared_ptr<C2SecureModeTuning> mSecureBufferMode;
    std::shared_ptr<C2GlobalLowLatencyModeTuning> mLowLatencyMode;
    std::shared_ptr<C2Avc4kMMU::input> mAvc4kMMUMode;
    std::shared_ptr<C2VendorGameModeLatency::input> mVendorGameModeLatency;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormatInfo;
    std::shared_ptr<C2ErrorPolicy::input> mErrorPolicy;

    c2_status_t mInitStatus;
    media::VideoCodecProfile mCodecProfile;
    InputCodec mInputCodec;
    C2VdecComponent *mComponent;
    bool mSecureMode;
    friend C2VdecComponent;

    // Declare the format configuration parameters according to different formats.
    void onAvcDeclareParam();
    void onHevcDeclareParam();
    void onVp9DeclareParam();
    void onAv1DeclareParam();
    void onDvheDeclareParam();
    void onDvavDeclareParam();
    void onDvav1DeclareParam();
    void onMp2vDeclareParam();
    void onMp4vDeclareParam();
    void onMjpgDeclareParam();
    void onAvsDeclareParam();
    void onAvs2DeclareParam();
    void onAvs3DeclareParam();
    void onVc1DeclareParam();

    // Declare the configuration parameters before playback.
    void onHdrDeclareParam(const std::shared_ptr<C2ReflectorHelper>& helper);
    void onApiFeatureDeclareParam();
    void onFrameRateDeclareParam();
    void onUnstablePtsDeclareParam();
    void onOutputDelayDeclareParam();
    void onInputDelayDeclareParam();
    void onTunnelDeclareParam();
    void onPipelineDelayDeclareParam();
    void onReorderBufferDepthDeclareParam();
    void onTunerPassthroughDeclareParam();
    void onGameModeLatencyDeclareParam();
    void onBufferSizeDeclareParam(const char* mine);
    void onBufferPoolDeclareParam();
    void onColorAspectsDeclareParam();
    void onLowLatencyDeclareParam();
    void onPixelFormatDeclareParam();
    void onVendorExtendParam();
    void onAvc4kMMUEnable();

    // These functions are used for component reconfiguration parameters before playback.
    void onTunneledModeTuningConfigParam();
    void onVendorTunerHalConfigParam();
    void onTunerPassthroughTrickModeConfigParam();
    void onTunerPassthroughWorkModeConfigParam();
    void onTunerPassthroughInstanceNoConfigParam();
    void onTunerPassthroughEventMaskConfigParam();
    void onVdecWorkModeConfigParam();
    void onDataSourceTypeConfigParam();
    void onStreamModeInputDelayConfigParam();
    void onStreamModePipeLineDelayConfigParam();
    void onStreamTunnelStartRenderConfigParam();
    void onStreamHdr10PlusInfoConfigParam();
    void onStreamMaxBufferSizeInfoConfigParam();
    void onNetflixVPeekConfigParam();
    void onErrorPolicyConfigParam();

    bool onStreamFrameRateConfigParam(std::vector<std::unique_ptr<C2SettingResult>>* const failures, C2Param* const param);

    c2_status_t onStreamPictureSizeConfigParam(std::vector<std::unique_ptr<C2SettingResult>>* const failures, C2Param* const param);

};
}

#endif /* _C2_Vdec_INTERFACE_IMPL_H_ */
