#ifndef _C2VENC_INTF_IMPL_H_
#define _C2VENC_INTF_IMPL_H_

#include "C2VencComp.h"
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>
#include "AmlVencParamIntf.h"


namespace android {


class C2VencComp::IntfImpl : public SimpleInterface<void>::BaseParams{
public:
    explicit IntfImpl(C2String name,C2String mimetype,const std::shared_ptr<C2ReflectorHelper> &helper);
    ~IntfImpl();
public:
    IAmlVencParam *GetVencParam();
    void onAvcProfileLevelParam();

    void onHevcProfileLevelParam();

    void onSizeParam(C2String mimetype);

    void onSVCParam();

public://static
    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::input> &oldMe,
        C2P<C2StreamPictureSizeInfo::input> &me);

    static C2R FramerateSetter(bool mayBlock, C2P<C2StreamFrameRateInfo::output> &me);

    static C2R MaxSizeCalculator(bool mayBlock, C2P<C2StreamMaxBufferSizeInfo::input>& me);

    static C2R LayerCountSetter(bool mayBlock, C2P<C2StreamTemporalLayeringTuning::output> &me);

    static C2R BitrateSetter(bool mayBlock, C2P<C2StreamBitrateInfo::output> &me);

    static C2R AvcProfileLevelSetter(
            bool mayBlock,
            C2P<C2StreamProfileLevelInfo::output> &me,
            const C2P<C2StreamPictureSizeInfo::input> &size,
            const C2P<C2StreamFrameRateInfo::output> &frameRate,
            const C2P<C2StreamBitrateInfo::output> &bitrate);

    static C2R HevcProfileLevelSetter(
            bool mayBlock,
            C2P<C2StreamProfileLevelInfo::output> &me,
            const C2P<C2StreamPictureSizeInfo::input> &size,
            const C2P<C2StreamFrameRateInfo::output> &frameRate,
            const C2P<C2StreamBitrateInfo::output> &bitrate);

    static C2R IntraRefreshSetter(bool mayBlock, C2P<C2StreamIntraRefreshTuning::output> &me);

    static C2R GopSetter(bool mayBlock, C2P<C2StreamGopTuning::output> &me);

    static C2R PictureQuantizationSetter(bool mayBlock,
                                         C2P<C2StreamPictureQuantizationTuning::output> &me);

    static C2R ColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::input> &me);

    static C2R CodedColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::output> &me,
                                       const C2P<C2StreamColorAspectsInfo::input> &coded);

   static C2R StrictValueWithNoDeps(bool mayBlock, C2P<C2StreamPixelFormatInfo::input> &old,
                                          C2P<C2StreamPixelFormatInfo::input> &me);

public:
    virtual c2_status_t config(
            const std::vector<C2Param*> &params, c2_blocking_t mayBlock,
            std::vector<std::unique_ptr<C2SettingResult>>* const failures,
            bool updateParams = true,
            std::vector<std::shared_ptr<C2Param>> *changes = nullptr);

public:
    void setAverageQp(int value) {mAverageBlockQuantization->value = value;}
    void setPictureType(C2Config::picture_type_t type) {mPictureType->value = type;}
    void setSyncFrameReq(bool req) {mRequestSync->value = req;}
    void SetPicSize(int width,int height) {mSize->width = width;mSize->height =height;};
    void setPitcureQuantLimit(int min,int max) {mPictureQuantization->m.values[0].min = min; mPictureQuantization->m.values[0].max = max;};
private:
    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    std::shared_ptr<C2StreamUsageTuning::input> mUsage;
    std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestSync;
    std::shared_ptr<C2StreamIntraRefreshTuning::output> mIntraRefresh;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
    std::shared_ptr<C2StreamProfileLevelInfo::output> mProfileLevel;
    std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> mSyncFramePeriod;
    std::shared_ptr<C2StreamGopTuning::output> mGop;
    std::shared_ptr<C2StreamPictureQuantizationTuning::output> mPictureQuantization;
    std::shared_ptr<C2StreamColorAspectsInfo::input> mColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mCodedColorAspects;
    std::shared_ptr<C2StreamPixelFormatInfo::input> mPixelFormat;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
    std::shared_ptr<C2VencCanvasMode::input> mVencCanvasMode;
    std::shared_ptr<C2PrependHeaderModeSetting> mPrependHeader;
    std::shared_ptr<C2StreamTemporalLayeringTuning::output> mLayerCount;
    std::shared_ptr<C2AndroidStreamAverageBlockQuantizationInfo::output> mAverageBlockQuantization;
    std::shared_ptr<C2StreamPictureTypeInfo::output> mPictureType;

private:
    IAmlVencParam *mAmlVencParam;
public:
    void ParamUpdate();
    static bool VencParamChangeListener(void *pInst,stParamChangeIndex Index,void *pParam);
};


}

#endif
