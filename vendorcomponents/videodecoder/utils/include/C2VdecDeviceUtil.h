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

#ifndef _C2_Vdec__METADATA_UTIL_H_
#define _C2_Vdec__METADATA_UTIL_H_

#include <mutex>
#include <queue>

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>
#include <SimpleC2Interface.h>

#include <util/C2InterfaceHelper.h>
#include <cutils/native_handle.h>
#include <C2VdecComponent.h>
#include <VideoDecWraper.h>

namespace android {

#define CODEC_MODE(a, b, c, d)\
	(((unsigned char)(a) << 24) | ((unsigned char)(b) << 16) | ((unsigned char)(c) << 8) | (unsigned char)(d))

#define META_DATA_MAGIC CODEC_MODE('M', 'E', 'T', 'A')
#define AML_META_HEAD_NUM  (8)
#define AML_META_HEAD_SIZE (AML_META_HEAD_NUM * sizeof(uint32_t))
#define UVM_META_DATA_VF_BASE_INFOS (1 << 0)
#define UVM_META_DATA_HDR10P_DATA (1 << 1)
#define META_DATA_SIZE 512

struct aml_meta_head_s {
    uint32_t magic;
    uint32_t type;
    uint32_t data_size;
    uint32_t data[5];
};

struct aml_vf_base_info_s {
    uint32_t width;
    uint32_t height;
    uint32_t duration;
    uint32_t frame_type;
    uint32_t type;
    uint32_t data[12];
};

struct aml_meta_info_s {
    union {
        struct aml_meta_head_s head;
        uint32_t buf[AML_META_HEAD_NUM];
    };
    unsigned char data[0];
};

enum useP010Mode_t {
    kUnUseP010 = 0,
    kUseSoftwareP010,
    kUseHardwareP010,
};

class C2VdecComponent::DeviceUtil : public IC2Observer {
public:
    DeviceUtil(bool secure);
    virtual ~DeviceUtil();

    c2_status_t setComponent(std::shared_ptr<C2VdecComponent> sharecomp);
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

    bool checkSupport8kMode();
    bool paramsPreCheck(std::shared_ptr<C2VdecComponent::IntfImpl> intfImpl);
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
    bool isReallocateOutputBuffer(VideoFormat rawFormat,VideoFormat currentFormat,
                                 bool *sizechange, bool *buffernumincrease);
    bool getMaxBufWidthAndHeight(uint32_t &width, uint32_t &height);
    bool getUvmMetaData(int fd,unsigned char *data,int *size);
    void parseAndProcessMetaData(unsigned char *data, int size, C2Work& work);
    bool parseAndProcessDuration(unsigned char *data, int size);
    void updateHDR10plusToWork(unsigned char *data, int size, C2Work& work);
    void updateDurationUs(unsigned char *data, int size);
    bool getHDR10PlusData(std::string &data);
    void setHDRStaticColorAspects(std::shared_ptr<C2StreamColorAspectsInfo::output> coloraspect);
    int32_t getDoubleWriteModeValue();
    int32_t getTripleWriteModeValue();

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

    bool checkConfigInfoFromDecoderAndReconfig(int type);
    uint32_t checkUseP010Mode(); /* 10bit */

    void setGameMode(bool enable);
    bool isLowLatencyMode();
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

    // The hardware platform supports 10bit decoding,
    // so use the triple write configuration parameter.
    int32_t getPropertyTripleWrite();
    uint64_t getUsageFromTripleWrite(int32_t triplewrite);

    int32_t getPropertyDoubleWrite();
    uint64_t getUsageFromDoubleWrite(int32_t doublewrite);
    bool checkDvProfileAndLayer();
    bool isYcrcb420Stream() const; /* 8bit */

    // It used to check if the dw and tw values,
    // are set to 10bit output.
    bool mIsNeedUse10BitOutBuffer;

    bool mIsYcbRP010Stream;
    bool mHwSupportP010;
    bool mSwSupportP010;

    //di post
    bool needDecoderReplaceBufferForDiPost();
    bool isUseVdecCore();

    int HDRInfoDataBLEndianInt(int value);

    std::weak_ptr<C2VdecComponent::IntfImpl> mIntfImpl;
    std::weak_ptr<VideoDecWraper> mVideoDecWraper;
    mediahal_cfg_parms* mConfigParam;
    std::weak_ptr<C2VdecComponent> mComp;
    bool mUseSurfaceTexture;
    bool mNoSurface;
    bool mHDRStaticInfoChanged;
    bool mHDR10PLusInfoChanged;
    bool mColorAspectsChanged;
    bool mSecure;
    bool mEnableNR;
    bool mDiPost;
    bool mForceDIPermission;
    bool mEnableDILocalBuf;
    bool mIs8k;
    bool mEnable8kNR;
    bool mDisableErrPolicy;
    bool mForceFullUsage;
    int  mBufMode;

    /* for low-latency mode */
    bool mUseLowLatencyMode;

    /* for check pts */
    bool mIsInterlaced;
    bool mEnableAvc4kMMU;
    uint32_t mAVCMMUWidth;
    uint32_t mAVCMMUHeight;

    uint32_t mDurationUs;
    uint32_t mDurationUsFromApp;
    float    mFramerate;
    bool     mCredibleDuration;
    int32_t  mUnstablePts;
    int32_t  mPlayerId;
    uint64_t mLastOutPts;
    uint64_t mOutputWorkCount;

    int32_t mMarginBufferNum;
    int32_t mStreamBitDepth;
    uint32_t mBufferWidth;
    uint32_t mBufferHeight;
    int32_t mMemcMode;

    /* for hdr10 plus */
    std::queue<std::string> mHDR10PlusData;
    std::unique_ptr<C2StreamHdrDynamicMetadataInfo::output> mHdr10PlusInfo = nullptr;
    bool mHaveHdr10PlusInStream;

    uint32_t mSignalType;
    bool mEnableAdaptivePlayback;
    std::mutex mMutex;
};

}

#endif
