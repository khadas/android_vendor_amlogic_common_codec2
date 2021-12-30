#ifndef _C2_VDA_COMPONENT_METADATA_UTIL_H_
#define _C2_VDA_COMPONENT_METADATA_UTIL_H_

#include <mutex>

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>
#include <C2VDAComponent.h>

#include <VideoDecWraper.h>
#include <amuvm.h>
#include <queue>

namespace android {
struct aml_stream_info {
    uint32_t width;
    uint32_t height;
    uint32_t max_width;
    uint32_t max_height;
    uint64_t pts_0;
    uint64_t pts_1;
    uint64_t pts_2;
    int len_0;
    int len_1;
    int len_2;
    int delay_time;
};

class C2VDAComponent::MetaDataUtil {
public:
    MetaDataUtil(C2VDAComponent* comp, bool secure);
    virtual ~MetaDataUtil();

    /* configure decoder */
    void codecConfig(mediahal_cfg_parms* params);
    void updateDecParmInfo(aml_dec_params* params);
    void flush();
    void updateInterlacedInfo(bool isInterlaced);
    bool isInterlaced() {return mIsInterlaced;};
    int getVideoType();
    void setUseSurfaceTexture(bool usersftexture) { mUseSurfaceTexture = usersftexture; }
    void setNoSurface(bool isNoSurface);
    bool isHDRStaticInfoUpdated() {
        if (mHDRStaticInfoChanged) {
            std::lock_guard<std::mutex> lock(mMutex);
            mHDRStaticInfoChanged = false;
            return true;
        }

        return false;
    }
    bool isHDR10PlusStaticInfoUpdated() {
        if (mHDR10PLusInfoChanged) {
            std::lock_guard<std::mutex> lock(mMutex);
            mHDR10PLusInfoChanged = false;
            return true;
        }
        return false;
    }

    bool isColorAspectsChanged() {
        if (mColorAspectsChanged) {
            std::lock_guard<std::mutex> lock(mMutex);
            mColorAspectsChanged = false;
            return true;
        }

        return false;
    }
    /* check and adjust out pts */
    int64_t checkAndAdjustOutPts(C2Work* work, int32_t flags);
    //int check_color_aspects();
    uint64_t getPlatformUsage();
    uint32_t getOutAlignedSize(uint32_t size, bool forcealign = false);
    bool getNeedReallocBuffer();
    bool checkReallocOutputBuffer(VideoFormat video_format_old,VideoFormat old_video_format_new);
    bool getMaxBufWidthAndHeight(uint32_t *width, uint32_t *height);
    bool getUvmMetaData(int fd,unsigned char *data,int *size);
    void parseAndprocessMetaData(unsigned char *data, int size);
    void updateHDR10plus(unsigned char *data, int size);
    void updateDurationUs(unsigned char *data, int size);
    bool getHDR10PlusData(std::string &data);
    void setHDRStaticColorAspects(std::shared_ptr<C2StreamColorAspectsInfo::output> coloraspect) {
        mHDRStaticInfoColorAspects = coloraspect;
    }
    int32_t getUnstablePts();
    int64_t getLastOutputPts();

    void save_stream_info(uint64_t timestamp, int filledlen);
    void check_stream_info();

    aml_stream_info mAmlStreamInfo;
private:
    /* set hdr static to decoder */
    int setHDRStaticInfo();
    int checkHDRMetadataAndColorAspects(struct aml_vdec_hdr_infos* phdr);
    int checkHdrStaticInfoMetaChanged(struct aml_vdec_hdr_infos* phdr);
    int isHDRStaticInfoDifferent(struct aml_vdec_hdr_infos* phd_old, struct aml_vdec_hdr_infos* phdr_new);
    // as C2Info in decoded output buffers.
    std::shared_ptr<C2StreamColorAspectsInfo::output> mHDRStaticInfoColorAspects;

    // static
    static const uint32_t kOutPutPtsValidNum = 5;

    int mUvmFd;
    C2VDAComponent::IntfImpl* mIntfImpl;
    mediahal_cfg_parms* mConfigParam;
    C2VDAComponent* mComp;
    bool mUseSurfaceTexture;
    bool mNoSurface;
    bool mHDRStaticInfoChanged;
    bool mHDR10PLusInfoChanged;
    bool mColorAspectsChanged;
    bool mSecure;
    bool mEnableNR;
    bool mEnableDILocalBuf;
    bool mEnable8kNR;
    bool mDisableErrPolicy;

    /* for check pts */
    bool mIsInterlaced;
    bool mInPtsInvalid;
    bool mFirstOutputWork;
    bool mOutputPtsValid;

    uint32_t mDurationUs;
    bool     mCredibleDuration;
    int32_t  mUnstablePts;
    uint64_t mLastOutPts;
    uint64_t mInPutWorkCount;
    uint64_t mOutputWorkCount;
    int32_t  mLastbitStreamId;
    int32_t  mOutputPtsValidCount;

    /* for hdr10 plus */
    std::queue<std::string> mHDR10PlusData;

    uint32_t mSignalType;
    bool mEnableAdaptivePlayback;
    std::mutex mMutex;
};

}

#endif
