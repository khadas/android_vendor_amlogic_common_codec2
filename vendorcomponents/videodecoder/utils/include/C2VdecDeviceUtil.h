#ifndef _C2_Vdec__METADATA_UTIL_H_
#define _C2_Vdec__METADATA_UTIL_H_

#include <mutex>

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>

#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>
#include <cutils/native_handle.h>
#include <C2VdecComponent.h>

#include <VideoDecWraper.h>
#include <amuvm.h>
#include <queue>

namespace android {

class C2VdecComponent::DeviceUtil {
public:
    DeviceUtil(C2VdecComponent* comp, bool secure);
    virtual ~DeviceUtil();

    /* configure decoder */
    void codecConfig(mediahal_cfg_parms* params);
    void updateDecParmInfo(aml_dec_params* params);
    void flush();
    void updateInterlacedInfo(bool isInterlaced);
    bool isInterlaced() {return mIsInterlaced;};
    int getVideoType();

    void setNoSurface(bool isNoSurface) { mNoSurface = isNoSurface;}
    void setForceFullUsage(bool isFullUsage) { mForceFullUsage = isFullUsage;}
    void setUseSurfaceTexture(bool userSurfaceTexture) { mUseSurfaceTexture = userSurfaceTexture;}

    int32_t getPlayerId() {return mPlayerId;}
    uint32_t getVideoDurationUs() {return mDurationUs;}
    int32_t getMarginBufferNum() {return mMarginBufferNum;}

    bool isHDRStaticInfoUpdated();
    bool isHDR10PlusStaticInfoUpdated();
    bool isColorAspectsChanged();

    //int check_color_aspects();
    uint64_t getPlatformUsage();
    uint32_t getOutAlignedSize(uint32_t size, bool forceAlign = false);
    bool needAllocWithMaxSize();
    bool checkReallocOutputBuffer(VideoFormat rawFormat,VideoFormat currentFormat,
                                 bool *sizechange, bool *buffernumincrease);
    bool getMaxBufWidthAndHeight(uint32_t &width, uint32_t &height);
    bool getUvmMetaData(int fd,unsigned char *data,int *size);
    void parseAndProcessMetaData(unsigned char *data, int size, C2Work& work);
    void updateHDR10plusToWork(unsigned char *data, int size, C2Work& work);
    void updateDurationUs(unsigned char *data, int size);
    bool getHDR10PlusData(std::string &data);
    void setHDRStaticColorAspects(std::shared_ptr<C2StreamColorAspectsInfo::output> coloraspect);
    uint32_t getDoubleWriteModeValue();

    // bit depth
    void queryStreamBitDepth();
    uint32_t getStreamPixelFormat(uint32_t pixelFormat);

    uint64_t getLastOutputPts();
    void setLastOutputPts(uint64_t);
    bool setUnstable();
    bool setDuration();
    bool shouldEnableMMU();
    bool clearDecoderDuration();
    bool updateDisplayInfoToGralloc(const native_handle_t* handle, int videoType, uint32_t sequenceNum);
    int setVideoDecWraper(VideoDecWraper* videoDecWraper);

    bool checkConfigInfoFromDecoderAndReconfig(int type);

private:
    void init(bool secure);
    /* set hdr static to decoder */
    int setHDRStaticInfo();
    int checkHDRMetadataAndColorAspects(struct aml_vdec_hdr_infos* phdr);
    int checkHdrStaticInfoMetaChanged(struct aml_vdec_hdr_infos* phdr);
    int isHDRStaticInfoDifferent(struct aml_vdec_hdr_infos* phd_old, struct aml_vdec_hdr_infos* phdr_new);
    // as C2Info in decoded output buffers.
    std::shared_ptr<C2StreamColorAspectsInfo::output> mHDRStaticInfoColorAspects;

    // static
    static const uint32_t kOutPutPtsValidNum = 5;

    int32_t getPropertyDoubleWrite();
    uint64_t getUsageFromDouleWrite(uint32_t doublewrite);
    bool checkDvProfileAndLayer();
    bool isYcrcb420Stream() const; /* 8bit */
    bool isYcbcRP010Stream() const; /* 10bit */

    int HDRInfoDataBLEndianInt(int value);

    int mUvmFd;
    C2VdecComponent::IntfImpl* mIntfImpl;
    VideoDecWraper* mVideoDecWraper;
    mediahal_cfg_parms* mConfigParam;
    C2VdecComponent* mComp;
    bool mUseSurfaceTexture;
    bool mNoSurface;
    bool mHDRStaticInfoChanged;
    bool mHDR10PLusInfoChanged;
    bool mColorAspectsChanged;
    bool mSecure;
    bool mEnableNR;
    bool mForceDIPermission;
    bool mEnableDILocalBuf;
    bool mIs8k;
    bool mEnable8kNR;
    bool mDisableErrPolicy;
    bool mForceFullUsage;

    /* for check pts */
    bool mIsInterlaced;
    bool mEnableAvc4kMMU;

    uint32_t mDurationUs;
    uint32_t mDurationUsFromApp;
    bool     mCredibleDuration;
    int32_t  mUnstablePts;
    int32_t  mPlayerId;
    uint64_t mLastOutPts;
    uint64_t mOutputWorkCount;

    int32_t mMarginBufferNum;
    int32_t mStreamBitDepth;
    uint32_t mBufferWidth;
    uint32_t mBufferHeight;

    /* for hdr10 plus */
    std::queue<std::string> mHDR10PlusData;

    uint32_t mSignalType;
    bool mEnableAdaptivePlayback;
    std::mutex mMutex;
};

}

#endif
