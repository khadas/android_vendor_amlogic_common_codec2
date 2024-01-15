#ifndef __AML_VENC_PARAM_INTF_H_
#define __AML_VENC_PARAM_INTF_H_

#include <C2Config.h>

namespace android {

typedef enum _ParamChangeIndex {
    E_AVRAGE_QP = 0,
    E_PIC_SIZE,
    E_FRAME_TYPE,
    E_SYNCFRAME_REQUEST,
}stParamChangeIndex;

typedef bool (*ParamChangeNotify)(void *pInst,stParamChangeIndex Index,void *pParam);

typedef struct _PicSize{
    int width;
    int height;
}stPicSize;

typedef struct _PicAlign {
    int width;
    int height;
}stPicAlign;

typedef enum _PictureType {
    IDR_FRAME = 0,
    I_FRAME,
    P_FRAME,
    B_FRAME
}ePicType;


typedef enum _CodecType {
    H264 = 0,
    H265
}eCodecType;


typedef struct _SVCInfo {
    bool enable;
    int SupportLayerCntMin;
    int SupportLayerCntMax;
    int LayerCnt;
}stSVCInfo;


typedef struct _QpInfo {
    bool enable;
    int QpMin;
    int QpMax;
    int iMin;
    int iMax;
    int pMin;
    int pMax;
    int bMin;
    int bMax;
}stQpInfo;

typedef struct _ColorAspect {
    C2Color::range_t range;
    C2Color::primaries_t primaries;
    C2Color::transfer_t transfer;
    C2Color::matrix_t matrix;
}stColorAspect;

typedef struct _ProfileLevelInfo {
    C2Config::profile_t profile;
    C2Config::level_t   level;
}stProfileLevelInfo;


typedef struct _VencParam {
    int Width;
    int Height;
    int Bitrate;
    int FrameRate;
    int PixFormat;
    bool SyncFrameRequest;
    uint64_t SyncFramePeriod;
    stQpInfo QpInfo;
    int LayerCnt;
    bool prepend_header_flag;
    bool canvas_mode;
    stColorAspect ColorAspect;
    stProfileLevelInfo ProfileLevel;
    char reserved[64];
}stVencParam;



class IAmlVencParam {

public:
    IAmlVencParam(){};
    virtual ~IAmlVencParam(){};

public:
    virtual bool RegisterChangeNotify(ParamChangeNotify Notifier,void *Param) = 0;
    virtual int GetMaxSupportInstance() = 0;
    virtual bool GetBitrateLimit(int &pMin,int &pMax) = 0;
    virtual bool GetSizeLimit(stPicSize &pMin,stPicSize &pMax,stPicAlign &align) = 0;
    virtual bool GetIsSupportDMAMode() = 0;
    virtual bool GetIsSupportCanvasMode() = 0;
    virtual bool GetSVCLimit(stSVCInfo &SvcInfo) = 0;
    virtual bool GetGopLimit(int &pMin,int &pMax) = 0;
    virtual bool GetQpLimit(int &Min,int &Max) = 0;
    virtual bool GetDefaultProfileLevel(C2Config::profile_t &profile,C2Config::level_t       &level) = 0;
    virtual int GetMaxOutputBufferSize() = 0;
    virtual void GetVencParam(stVencParam &Param) = 0;
    virtual void SetSize(int width,int height) = 0;
    virtual void SetBitrate(int bitrate) = 0;
    virtual void SetSyncPeriodSize(uint64_t Period) = 0;
    virtual void SetFrameRate(int FrameRate) = 0;
    virtual void SetQpInfo(stQpInfo QpInfo) = 0;
    virtual void SetLayerCnt(int LayerCount) = 0;
    virtual void SetPixFormat(int Format) = 0;
    virtual void SetSyncFrameRequest(bool Request) = 0;
    virtual void SetPrependFlag(bool Flag) = 0;
    virtual void SetCanvasMode(bool Canvas) = 0;
    virtual void SetColorAspect(stColorAspect ColorAspect) = 0;
    virtual void SetProfileLevel(stProfileLevelInfo Info) = 0;
    virtual bool UpdateAvgQp(int AvgQp) = 0;
    virtual bool UpdateFrameType(ePicType FrameType) = 0;
    virtual bool UpdateSyncFrameReq(int Request) = 0;
    virtual bool UpdatePicSize(int Width,int Height) = 0;
    virtual void SetCodecType(eCodecType CodecType) = 0;
    virtual eCodecType GetCodecType() = 0;

};

extern "C" IAmlVencParam *VencParamGetInstance();
extern "C" void VencParamDelInstance(IAmlVencParam *pInstance);


}



#endif
