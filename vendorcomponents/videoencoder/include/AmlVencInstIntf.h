#ifndef __AML_VENC_INST_INTF_H_
#define __AML_VENC_INST_INTF_H_

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>
#include "AmlVencParamIntf.h"

namespace android {

typedef struct OutputFrame {
    ePicType FrameType;
    uint8_t *Data;
    uint32_t Length;
}stOutputFrame;

typedef enum _Result {
    SUCCESS = 0,
    FAIL,
    WORK_IS_VALID,
}eResult;

typedef enum _BufferType {
    VMALLOC,
    DMA,
    CANVAS
}eBufferType;


typedef struct _InputFrameInfo{
    uint8_t *yPlane;
    uint8_t *uPlane;
    uint8_t *vPlane;
    int colorFmt;
    uint64_t frameIndex;
    uint64_t timeStamp;
    int32_t yStride;
    int32_t uStride;
    int32_t vStride;
    int32_t HStride; //for height stride
    eBufferType bufType;
    int shareFd[3];
    int planeNum;
    uint64_t canvas;
    int size;
    uint8_t reserved[64];
}stInputFrameInfo;


class IAmlVencInst {

public:
    IAmlVencInst() {};
    virtual ~IAmlVencInst() {};

public:
    virtual bool init() = 0;
    virtual bool GenerateHeader(char *pHeader,unsigned int &Length) = 0;
    virtual eResult PreProcess(std::shared_ptr<C2Buffer> inputBuffer,stInputFrameInfo &InputFrameInfo) = 0;
    virtual c2_status_t ProcessOneFrame(stInputFrameInfo InputFrameInfo,stOutputFrame &OutFrame) = 0;
    virtual bool Destroy() = 0;
    virtual void SetVencParamInst(IAmlVencParam *pVencParamInst) = 0;

public:
    static IAmlVencInst *GetInstance();
    static void DelInstance(IAmlVencInst *pInstance);


};



}





#endif
