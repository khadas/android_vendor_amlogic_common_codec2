// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VENC_COMP_H
#define ANDROID_C2_VENC_COMP_H

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
#include <c2logdebug.h>
#include <C2VendorConfig.h>

#include <SimpleC2Interface.h>
//#include <util/C2InterfaceHelper.h>
#include "ThreadWorker.h"
#include <media/stagefright/foundation/Mutexed.h>
#include <buffer.h>
#include "AmlVencInstIntf.h"

namespace android {

typedef IAmlVencInst* (*C2VencCreateInstance)();
typedef void (*C2VencDestroyInstance)(IAmlVencInst*);

class C2VencComp : public C2Component ,
                                 public std::enable_shared_from_this<C2VencComp> {
public:
    class IntfImpl;
    /*static std::shared_ptr<C2Component> create(const std::string& name, c2_node_id_t id,
                                               const std::shared_ptr<C2ReflectorHelper>& helper,
                                               C2ComponentFactory::ComponentDeleter deleter);
    */
    C2VencComp(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2VencComp() override;

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
    virtual bool LoadModule(){return true;};
    virtual c2_status_t Init(){return C2_OK;};
    //virtual c2_status_t ProcessOneFrame(InputFrameInfo_t InputFrameInfo,OutputFrameInfo_t *pOutFrameInfo){return C2_OK;};
    virtual c2_status_t GenerateHeader(uint8_t *pHeaderData,uint32_t *pSize){return C2_OK;};
    virtual void Close(){};
    // The pointer of component listener.
public:
    //static
    static std::atomic<int32_t> sConcurrentInstances;
    static uint32_t mInstanceID;
    // static
    static std::shared_ptr<C2Component> create(
            char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &mIntfImpl);
private:
    std::shared_ptr<C2Buffer> createLinearBuffer(
                             const std::shared_ptr<C2LinearBlock> &block, size_t offset, size_t size);
    std::shared_ptr<C2Buffer> createGraphicBuffer(
            const std::shared_ptr<C2GraphicBlock> &block,
            const C2Rect &crop);
    bool doSomeInit();
    void ProcessData();
    c2_status_t stop_process();
    void ConfigParam(std::unique_ptr<C2Work> &work);
    void finishWork(uint64_t workIndex, std::unique_ptr<C2Work> &work,stOutputFrame OutFrameInfo);
    void finish(uint64_t frameIndex, std::function<void(std::unique_ptr<C2Work> &)> fillWork);
    void WorkDone(std::unique_ptr<C2Work> &work);
    bool Load();
    void unLoad();
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
    std::shared_ptr<IntfImpl> mIntfImpl;
    const std::shared_ptr<C2ComponentInterface> mIntf;
    std::list<std::unique_ptr<C2Work>> mQueue;
    bool mIsInit;
    bool mSpsPpsHeaderReceived;
    uint32_t mOutBufferSize;
    bool mSawInputEOS;
    Mutex mInputQueueLock;
    Mutex mProcessDoneLock;
    Condition mProcessDoneCond;
    IAmlVencInst *mAmlVencInst;
    void* mLibHandle;
    C2VencCreateInstance CreateMethod;
    C2VencDestroyInstance DestroyMethod;
    Mutex mDestroyQueueLock;
};

}

#endif //ANDROID_C2_VENC_COMPONENT_H
