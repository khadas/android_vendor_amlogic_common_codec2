// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VENC_COMPONENT_H
#define ANDROID_C2_VENC_COMPONENT_H

//#include <VideoDecodeAcceleratorAdaptor.h>

//#include <video_codecs.h>
//#include <video_decode_accelerator.h>

#include <C2Component.h>
#include <C2ComponentFactory.h>
#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>
#include <C2VendorProperty.h>
#include <C2VendorDebug.h>
#include <C2VendorConfig.h>

#include <SimpleC2Interface.h>
//#include <util/C2InterfaceHelper.h>
#include "ThreadWorker.h"
#include <media/stagefright/foundation/Mutexed.h>
#include <am_gralloc_ext.h>
#include <C2VencLogDebug.h>

//#include <base/macros.h>
//#include <base/memory/ref_counted.h>
//#include <base/single_thread_task_runner.h>
//#include <base/synchronization/waitable_event.h>
//#include <base/threading/thread.h>

//#include <atomic>
//#include <deque>
//#include <map>
//#include <mutex>
//#include <queue>
//#include <unordered_map>

namespace android {

typedef enum FrameType {
    FRAMETYPE_IDR,
    FRAMETYPE_I,
    FRAMETYPE_P,
    FRAMETYPE_B
}FrameType_e;

typedef enum ColorFmt {
    C2_ENC_FMT_NV12,
    C2_ENC_FMT_NV21,
    C2_ENC_FMT_I420,
    C2_ENC_FMT_YV12,
    C2_ENC_FMT_RGBA8888
}ColorFmt_e;

typedef struct OutputFrameInfo {
    FrameType_e FrameType;
    uint8_t *Data;
    uint32_t Length;
}OutputFrameInfo_t;

typedef enum BufferType {
    VMALLOC,
    DMA,
	CANVAS
}BufferType_e;

typedef struct InputFrameInfo{
    uint8_t *yPlane;
    uint8_t *uPlane;
    uint8_t *vPlane;
    ColorFmt colorFmt;
    uint64_t frameIndex;
    uint64_t timeStamp;
    int32_t yStride;
    int32_t uStride;
    int32_t vStride;
    int32_t HStride; //for height stride
    BufferType_e bufType;
    int shareFd[3];
    int planeNum;
    uint64_t canvas;
    int size;
    bool needunmap;
}InputFrameInfo_t;

typedef enum DumpFileType {
    C2_DUMP_RAW,
    C2_DUMP_ES
}DumpFileType_e;

typedef struct DataModeInfo {
    unsigned long type;
    unsigned long pAddr;
    unsigned long canvas;
}DataModeInfo_t;

class C2VencComponent : public C2Component ,
                                 public std::enable_shared_from_this<C2VencComponent> {
public:
    //class IntfImpl;
    /*static std::shared_ptr<C2Component> create(const std::string& name, c2_node_id_t id,
                                               const std::shared_ptr<C2ReflectorHelper>& helper,
                                               C2ComponentFactory::ComponentDeleter deleter);
    */
    C2VencComponent(const std::shared_ptr<C2ComponentInterface> &intf);
    virtual ~C2VencComponent() override;

    //class MetaDataUtil;

    // Implementation of C2Component interface
    virtual c2_status_t setListener_vb(const std::shared_ptr<Listener>& listener,
                                       c2_blocking_t mayBlock) override;

    virtual c2_status_t queue_nb(std::list<std::unique_ptr<C2Work>>* const items) override;
    virtual c2_status_t announce_nb(const std::vector<C2WorkOutline>& items) override;
    virtual c2_status_t flush_sm(flush_mode_t mode,
                                 std::list<std::unique_ptr<C2Work>>* const flushedWork) override;
    virtual c2_status_t drain_nb(drain_mode_t mode) override;
    virtual c2_status_t start() override;
    virtual c2_status_t stop() override;
    virtual c2_status_t reset() override;
    virtual c2_status_t release() override;
    virtual std::shared_ptr<C2ComponentInterface> intf() override;

    static void *runWorkLoop(void *arg);
    void *threadLoop();

protected:
    virtual bool LoadModule() = 0;
    virtual c2_status_t Init() = 0;
    virtual c2_status_t ProcessOneFrame(InputFrameInfo_t InputFrameInfo,OutputFrameInfo_t *pOutFrameInfo) = 0;
    virtual c2_status_t GenerateHeader(uint8_t *pHeaderData,uint32_t *pSize) = 0;
    virtual void getResolution(int *pWidth,int *pHeight) = 0;
    virtual void getCodecDumpFileName(std::string &strName,DumpFileType_e type) = 0;
    virtual bool isSupportDMA() = 0;
    virtual bool isSupportCanvas() = 0;
    virtual void Close() = 0;
    // The pointer of component listener.
private:
    std::shared_ptr<C2Buffer> createLinearBuffer(
                             const std::shared_ptr<C2LinearBlock> &block, size_t offset, size_t size);

    std::shared_ptr<C2Buffer> createGraphicBuffer(
            const std::shared_ptr<C2GraphicBlock> &block,
            const C2Rect &crop);
    bool OpenFile(int *fd,char *pName);
    uint32_t dumpDataToFile(int fd,uint8_t *data,uint32_t size);
    bool doSomeInit();
    void ProcessData();
    c2_status_t stop_process();
    c2_status_t CanvasDataProc(DataModeInfo_t *pDataMode,InputFrameInfo *pFrameInfo);
    c2_status_t DMAProc(const native_handle_t*priv_handle,InputFrameInfo *pFrameInfo,uint32_t *dumpFileSize);
    c2_status_t ViewDataProc(std::shared_ptr<const C2GraphicView> view,InputFrameInfo *pFrameInfo,uint32_t *dumpFileSize);
    c2_status_t LinearDataProc(std::shared_ptr<const C2ReadView> view,InputFrameInfo *pFrameInfo);
    c2_status_t GraphicDataProc(std::shared_ptr<C2Buffer> inputBuffer,InputFrameInfo *pFrameInfo);
    c2_status_t CheckPicSize(std::shared_ptr<const C2GraphicView> view,uint64_t frameIndex);
    bool codecFmtTrans(uint32_t inputCodec,ColorFmt *pOutputCodec);
    void ConfigParam(std::unique_ptr<C2Work> &work);
    void finishWork(uint64_t workIndex, std::unique_ptr<C2Work> &work,OutputFrameInfo_t OutFrameInfo);
    void finish(uint64_t frameIndex, std::function<void(std::unique_ptr<C2Work> &)> fillWork);
    void WorkDone(std::unique_ptr<C2Work> &work);
    bool IsYUV420(const C2GraphicView &view);
    bool IsNV12(const C2GraphicView &view);
    bool IsNV21(const C2GraphicView &view);
    bool IsI420(const C2GraphicView &view);
    // The state machine enumeration on component thread.
    enum class ComponentState : int32_t {
        // This is the initial state until VDA initialization returns successfully.
        UNINITIALIZED,
        // VDA initialization returns successfully. VDA is ready to make progress.
        STARTED,
        // onDrain() is called. VDA is draining. Component will hold on queueing works until
        // onDrainDone().
        DRAINING,
        // onFlush() is called. VDA is flushing. State will change to STARTED after onFlushDone().
        FLUSHING,
        // onStop() is called. VDA is shutting down. State will change to UNINITIALIZED after
        // onStopDone().
        STOPPING,
        //when destructor is called, VDA is destroying.  state will change to DESTROYED after onDestroy
        DESTROYING,
        //after onDestroy is called, VDA is destroyed, state will change to DESTROYED
        DESTROYED,
    };

    class BlockingBlockPool;
    std::shared_ptr<BlockingBlockPool> mOutputBlockPool;
    std::shared_ptr<C2LinearBlock> mOutBlock;

    ThreadWorker mthread;
    ComponentState mComponentState;
    std::shared_ptr<Listener> mListener;
    // The pointer of component interface implementation.
    //std::shared_ptr<IntfImpl> mIntfImpl;
    // The pointer of component interface.
    const std::shared_ptr<C2ComponentInterface> mIntf;
    std::list<std::unique_ptr<C2Work>> mQueue;
    bool mIsInit;
    int mfdDumpInput;
    int mfdDumpOutput;
    bool mSpsPpsHeaderReceived;
    uint32_t mOutBufferSize;
    bool mSawInputEOS;
    Mutex mInputQueueLock;
    bool mDumpYuvEnable;
    bool mDumpEsEnable;
    Mutex mProcessDoneLock;
    Condition mProcessDoneCond;
};

}

#endif //ANDROID_C2_VENC_COMPONENT_H
