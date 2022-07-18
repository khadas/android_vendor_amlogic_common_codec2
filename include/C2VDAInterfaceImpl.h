#ifndef _C2_VDA_INTERFACE_IMPL_H_
#define _C2_VDA_INTERFACE_IMPL_H_

#include <C2VDAComponent.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>

namespace android {

class C2VDAComponent::IntfImpl : public C2InterfaceHelper {
public:
    IntfImpl(C2String name, const std::shared_ptr<C2ReflectorHelper>& helper);

    c2_status_t config(
            const std::vector<C2Param*> &params, c2_blocking_t mayBlock,
            std::vector<std::unique_ptr<C2SettingResult>>* const failures,
            bool updateParams = true,
            std::vector<std::shared_ptr<C2Param>> *changes = nullptr);

    // interfaces for C2VDAComponent
    c2_status_t status() const { return mInitStatus; }
    media::VideoCodecProfile getCodecProfile() { return mCodecProfile; }
    C2BlockPool::local_id_t getBlockPoolId() const { return mOutputBlockPoolIds->m.values[0]; }
    InputCodec getInputCodec() { return mInputCodec; }

    //for DolbyVision Only
    void updateInputCodec(InputCodec videotype) {
        if (InputCodec::DVHE <= videotype && videotype <= InputCodec::DVAV1){
            mInputCodec = videotype;
        }
    }
    void updateCodecProfile(media::VideoCodecProfile profile) {
        mCodecProfile = profile;
    }
    void setComponent(C2VDAComponent* comp) {mComponent = comp;}
    std::shared_ptr<C2StreamColorAspectsInfo::output> getColorAspects() {
        return this->mColorAspects;
    }
    float getInputFrameRate() {  return  mFrameRateInfo->value; }

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
    DECLARE_C2_DEFAUTL_UNSTRICT_SETTER(C2VendorTunerHalParam::input, VendorTunerHalParam);

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
    // Compnent uses this ID to fetch corresponding output block pool from platform.
    std::shared_ptr<C2PortBlockPoolsTuning::output> mOutputBlockPoolIds;
    // The color aspects parsed from input bitstream. This parameter should be configured by
    // component while decoding.
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
    std::shared_ptr<C2StreamHdr10PlusInfo::input> mHdr10PlusInfoInput;
    std::shared_ptr<C2StreamHdr10PlusInfo::output> mHdr10PlusInfoOutput;
    std::shared_ptr<C2PortActualDelayTuning::input> mActualInputDelay;

    //frame rate
    std::shared_ptr<C2StreamFrameRateInfo::input> mFrameRateInfo;
    //unstable pts
    std::shared_ptr<C2StreamUnstablePts::input> mUnstablePts;
    //stream mode
    std::shared_ptr<C2VdecWorkMode::input> mVdecWorkMode;
    //dmx source
    std::shared_ptr<C2DataSourceType::input> mDataSourceType;
    std::shared_ptr<C2PortActualDelayTuning::output> mActualOutputDelay;
    std::shared_ptr<C2ActualPipelineDelayTuning> mActualPipelineDelay;
    std::shared_ptr<C2PortReorderBufferDepthTuning> mReorderBufferDepth;

    //tunnel mode
    std::shared_ptr<C2PortTunneledModeTuning::output> mTunnelModeOutput;
    std::shared_ptr<C2PortTunnelHandleTuning::output> mTunnelHandleOutput;
    std::shared_ptr<C2PortTunnelSystemTime::output> mTunnelSystemTimeOut;
    std::shared_ptr<C2VendorTunerHalParam::input> mVendorTunerHalParam;

    std::shared_ptr<C2SecureModeTuning> mSecureBufferMode;
    std::shared_ptr<C2GlobalLowLatencyModeTuning> mLowLatencyMode;

    c2_status_t mInitStatus;
    media::VideoCodecProfile mCodecProfile;
    InputCodec mInputCodec;
    C2VDAComponent *mComponent;
    bool mSecureMode;
    friend C2VDAComponent;

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

    void onHdrDeclareParam(const std::shared_ptr<C2ReflectorHelper>& helper);
    void onApiFeatureDeclareParam();
    void onFrameRateDeclareParam();
    void onUnstablePtsDeclareParam();
    void onOutputDelayDeclareParam();
    void onInputDelayDeclareParam();
    void onTunnelDeclareParam();
    void onPipelineDelayDeclareParam();
    void onReorderBufferDepthDeclareParam();
    void onTunnelPassthroughDeclareParam();
    void onBufferSizeDeclareParam(const char* mine);
    void onBufferPoolDeclareParam();
    void onColorAspectsDeclareParam();
    void onLowLatencyDeclareParam();
    void onVendorExtendParam();
};
}

#endif /* _C2_VDA_INTERFACE_IMPL_H_ */
