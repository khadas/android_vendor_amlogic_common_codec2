/*
 * Copyright 2023 The Android Open Source Project
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


//#define LOG_NDEBUG 0
#define LOG_TAG "C2VencComp"

#include <C2VencComp.h>
#include <C2AllocatorGralloc.h>
#include <C2ComponentFactory.h>
#include <C2PlatformSupport.h>

#include <cutils/native_handle.h>
#include <media/stagefright/MediaDefs.h>

#include <ui/GraphicBuffer.h>
#include <utils/Log.h>
#include <cutils/properties.h>
#include "C2VendorSupport.h"
#include "C2VencIntfImpl.h"

#include <string>
#include <inttypes.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <stdio.h>
#include <dlfcn.h>


namespace android {

/** Used to remove warnings about unused parameters */
#define UNUSED(x) ((void)(x))

constexpr char COMPONENT_NAME_AVC[] = "c2.amlogic.avc.encoder";
constexpr char COMPONENT_NAME_HEVC[] = "c2.amlogic.hevc.encoder";

const C2String kComponentLoadMediaProcessLibrary = "lib_encoder_media_process.so";

#define OUTPUT_BUFFERSIZE_MIN (5 * 1024 * 1024)

#define C2Venc_LOG(level, fmt, str...) CODEC2_LOG(level, fmt, ##str)

class C2VencComp::BlockingBlockPool : public C2BlockPool {
public:
    BlockingBlockPool(const std::shared_ptr<C2BlockPool>& base): mBase{base} {}

    virtual local_id_t getLocalId() const override {
        return mBase->getLocalId();
    }

    virtual C2Allocator::id_t getAllocatorId() const override {
        return mBase->getAllocatorId();
    }

    virtual c2_status_t fetchLinearBlock(
            uint32_t capacity,
            C2MemoryUsage usage,
            std::shared_ptr<C2LinearBlock>* block) {
        c2_status_t status;
        do {
            status = mBase->fetchLinearBlock(capacity, usage, block);
        } while (status == C2_BLOCKING);
        return status;
    }

    virtual c2_status_t fetchCircularBlock(
            uint32_t capacity,
            C2MemoryUsage usage,
            std::shared_ptr<C2CircularBlock>* block) {
        c2_status_t status;
        do {
            status = mBase->fetchCircularBlock(capacity, usage, block);
        } while (status == C2_BLOCKING);
        return status;
    }

    virtual c2_status_t fetchGraphicBlock(
            uint32_t width, uint32_t height, uint32_t format,
            C2MemoryUsage usage,
            std::shared_ptr<C2GraphicBlock>* block) {
        c2_status_t status;
        do {
            status = mBase->fetchGraphicBlock(width, height, format, usage,
                                              block);
        } while (status == C2_BLOCKING);
        return status;
    }

private:
    std::shared_ptr<C2BlockPool> mBase;
};

// static
std::atomic<int32_t> C2VencComp::sConcurrentInstances = 0;

// static
std::shared_ptr<C2Component> C2VencComp::create(
        char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl>& helper) {
    static const int32_t kMaxConcurrentInstances = helper->GetVencParam()->GetMaxSupportInstance();//6;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (strcmp(name,COMPONENT_NAME_AVC) && strcmp(name,COMPONENT_NAME_HEVC)) {
        ALOGE("invalid component name %s",name);
        return nullptr;
    }
    if (kMaxConcurrentInstances >= 0 && sConcurrentInstances.load() >= kMaxConcurrentInstances) {
        ALOGE("Reject to Initialize() due to too many instances: %d", sConcurrentInstances.load());
        return nullptr;
    }
    return std::shared_ptr<C2Component>(new C2VencComp(name, id, helper));
}


C2VencComp::C2VencComp(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl)
                : mComponentState(ComponentState::UNINITIALIZED),
                  mIntfImpl(intfImpl),
                  mIntf(std::make_shared<SimpleInterface<IntfImpl>>(name, id, intfImpl)),
                  mIsInit(false),
                  mSpsPpsHeaderReceived(false),
                  mOutBufferSize(OUTPUT_BUFFERSIZE_MIN),
                  mSawInputEOS(false),
                  mAmlVencInst(NULL),
                  mLibHandle(NULL),
                  CreateMethod(NULL),
                  DestroyMethod(NULL) {
    ALOGD("C2VencComponent constructor!");
    propGetInt(CODEC2_VENC_LOGDEBUG_PROPERTY, &gloglevel);
    ALOGD("gloglevel:%x",gloglevel);
    ALOGD("mOutBufferSize:%d",mOutBufferSize);
    //mAmlVencInst = IAmlVencInst::GetInstance();
    Load();
    mAmlVencInst->SetVencParamInst(mIntfImpl->GetVencParam());
}

C2VencComp::~C2VencComp() {
    /*
     * This is the logic, no need to modify, ignore coverity weak cryptor report.
    */
    /*coverity[exn_spec_violation:SUPPRESS]*/
    if (mComponentState != ComponentState::UNINITIALIZED) {
        ALOGD("C2VencComponent client process exit,but not stop!now stop process!!!");
        stop_process();
    }
    unLoad();
    //IAmlVencInst::DelInstance(mAmlVencInst);
    ALOGD("C2VencComponent destructor!");
}

c2_status_t C2VencComp::setListener_vb(const std::shared_ptr<Listener>& listener,
                                           c2_blocking_t mayBlock) {
    UNUSED(mayBlock);
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"C2VencComponent setListener_vb!");
    mListener = listener;
    return C2_OK;
}

c2_status_t C2VencComp::queue_nb(std::list<std::unique_ptr<C2Work>>* const items) {
    C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"C2VencComponent queue_nb!,receive buffer count:%d", (int)items->size());
    AutoMutex l(mInputQueueLock);
    while (!items->empty()) {
        mQueue.push_back(std::move(items->front()));
        items->pop_front();
    }
    return C2_OK;
}

c2_status_t C2VencComp::announce_nb(const std::vector<C2WorkOutline> &items) {
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"C2VencComponent announce_nb!");

    return C2_OK;
}

c2_status_t C2VencComp::flush_sm(flush_mode_t mode, std::list<std::unique_ptr<C2Work>>* const flushedWork) {
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"C2VencComponent flush_sm!");
    (void)mode;
/*    {
        Mutexed<ExecState>::Locked state(mExecState);
        if (state->mState != RUNNING) {
            return C2_BAD_STATE;
        }
    }*/
    {
        // TODO: queue->splicedBy(flushedWork, flushedWork->end());
        while (!mQueue.empty()) {
            std::unique_ptr<C2Work> work = std::move(mQueue.front());
            mQueue.pop_front();
            if (work) {
                flushedWork->push_back(std::move(work));
            }
        }
    }
    return C2_OK;
}

c2_status_t C2VencComp::drain_nb(drain_mode_t mode) {
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"C2VencComponent drain_nb!");

    return C2_OK;
}


bool C2VencComp::Load() {
     mLibHandle = dlopen(kComponentLoadMediaProcessLibrary.c_str(), RTLD_NOW | RTLD_NODELETE);
    if (mLibHandle == nullptr) {
        ALOGD("Could not dlopen %s: %s", kComponentLoadMediaProcessLibrary.c_str(), dlerror());
        return false;
    }
    ALOGE("C2VencComp::mLibHandle:%p",mLibHandle);
    CreateMethod = (C2VencCreateInstance)dlsym(
                mLibHandle, "_ZN7android12IAmlVencInst11GetInstanceEv");

    DestroyMethod = (C2VencDestroyInstance)dlsym(
                mLibHandle, "_ZN7android12IAmlVencInst11DelInstanceEPS0_");

    if (CreateMethod) {
        mAmlVencInst = CreateMethod();
        if (NULL == mAmlVencInst) {
            ALOGE("mAmlVencInst is null");
            return false;
        }
    }
    else {
        ALOGE("CreateMethod is null");
        return false;
    }

    if (!DestroyMethod) {
        ALOGE("DestroyMethod is null");
        return false;
    }
    return true;
}


void C2VencComp::unLoad() {
    if (DestroyMethod) {
        ALOGE("Destroy mAmlVencInst");
        DestroyMethod(mAmlVencInst);
    }

    if (mLibHandle) {
        ALOGV("Unloading dll");
        dlclose(mLibHandle);
    }
}


c2_status_t C2VencComp::start() {
    std::string strFileName;
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"C2VencComponent start!");

    if (ComponentState::UNINITIALIZED != mComponentState) {
        C2Venc_LOG(CODEC2_VENC_LOG_ERR,"need go to start state,but current state is:%d,start failed",mComponentState);
        return C2_BAD_STATE;
    }

    if (doSomeInit()) {
        C2Venc_LOG(CODEC2_VENC_LOG_INFO,"Modul init successful!");
    }
    else {
        C2Venc_LOG(CODEC2_VENC_LOG_ERR,"Module init failed!!,please check");
        return C2_NO_INIT;
    }
    mthread.start(runWorkLoop,this);
    mComponentState = ComponentState::STARTED;

    return C2_OK;
}

c2_status_t C2VencComp::stop_process() {
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"C2VencComponent stop!");
    if (mComponentState == ComponentState::UNINITIALIZED) {
        C2Venc_LOG(CODEC2_VENC_LOG_ERR,"this component has already stopped");
        return C2_NO_INIT;
    }
    mComponentState = ComponentState::STOPPING;
    {
        AutoMutex l(mInputQueueLock);
        mQueue.clear();
    }
    if (mthread.isRunning()) {
        mthread.stop();
        C2Venc_LOG(CODEC2_VENC_LOG_INFO,"wait for thread to exit!");
        AutoMutex l(mProcessDoneLock);
        if (mProcessDoneCond.waitRelative(mProcessDoneLock,500000000ll) == ETIMEDOUT) {
            C2Venc_LOG(CODEC2_VENC_LOG_ERR,"wait for thread timeout!!!!");
        }
        else {
            C2Venc_LOG(CODEC2_VENC_LOG_INFO,"wait for thread exit done!!!!");
        }
    }
    mComponentState = ComponentState::UNINITIALIZED;
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"stop done,set state to UNINITIALIZED");
    return C2_OK;
}



c2_status_t C2VencComp::stop() {
    c2_status_t ret = C2_OK;

    ret = stop_process();
    {
        AutoMutex l(mDestroyQueueLock); //for kill user process upper than 1 time,need lock to protect
        if (mAmlVencInst) {
            C2Venc_LOG(CODEC2_VENC_LOG_INFO,"Destroy process,mAmlVencInst:%p",mAmlVencInst);
            mAmlVencInst->Destroy();
            mAmlVencInst = NULL;
        }
    }
    return ret;
}

c2_status_t C2VencComp::reset() {
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"C2VencComponent reset!");
    stop();
    if (mOutBlock) {
        mOutBlock.reset();
    }
    //Init(); //we will do init function in start process
    return C2_OK;
}

c2_status_t C2VencComp::release() {
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"C2VencComponent release!");
    stop();
    return C2_OK;
}

std::shared_ptr<C2ComponentInterface> C2VencComp::intf() {
    return mIntf;
}


bool C2VencComp::doSomeInit() {
    if (!mIsInit) {
        C2Venc_LOG(CODEC2_VENC_LOG_INFO,"now init encoder module");
        if (!mAmlVencInst->init()) {
            C2Venc_LOG(CODEC2_VENC_LOG_INFO,"doSomeInit init encoder failed!!");
            return false;
        }
    }
    return true;
}

void C2VencComp::ProcessData()
{
    std::unique_ptr<C2Work> work;
    eResult Result = FAIL;
    stInputFrameInfo InputFrameInfo;
    std::shared_ptr<const C2ReadView> Linearview;
    memset(&InputFrameInfo,0,sizeof(InputFrameInfo));

    {
        AutoMutex l(mInputQueueLock);
        if (mQueue.empty())
            return;
        C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"begin to process input data");
        work = std::move(mQueue.front());
        mQueue.pop_front();
    }

    if (NULL == work) {
        C2Venc_LOG(CODEC2_VENC_LOG_ERR,"NULL == work!!!!");
        return;
    }

    {
        std::vector<C2Param *> updates;
        for (const std::unique_ptr<C2Param> &param: work->input.configUpdate) {
            if (param) {
                updates.emplace_back(param.get());
            }
        }
        if (!updates.empty()) {
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            c2_status_t err = intf()->config_vb(updates, C2_MAY_BLOCK, &failures);
            C2Venc_LOG(CODEC2_VENC_LOG_ERR,"applied %zu configUpdates => %s (%d)", updates.size(), asString(err), err);
        }
    }

    if (!work->input.buffers.empty() && !work->input.buffers[0]) {
        C2Venc_LOG(CODEC2_VENC_LOG_ERR,"Encountered null input buffer. Clearing the input buffer");
        work->input.buffers.clear();
    }

    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;

    //std::shared_ptr<const C2GraphicView> view;

    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        C2Venc_LOG(CODEC2_VENC_LOG_ERR,"saw eos flag");
        mSawInputEOS = true;
    }

    if (work->input.buffers.empty()) {
        C2Venc_LOG(CODEC2_VENC_LOG_ERR,"input buffer list is empty");
        work->workletsProcessed = 1u;
        WorkDone(work);
        return;
    }

    //C2Handle *handle = inputBuffer->data.graphicBlocks().front().handle();
    std::shared_ptr<C2Buffer> inputBuffer;
    //std::shared_ptr<const C2GraphicView> view;

    inputBuffer = work->input.buffers[0];
    //view = std::make_shared<const C2GraphicView>(inputBuffer->data().graphicBlocks().front().map().get());

    InputFrameInfo.frameIndex = work->input.ordinal.frameIndex.peekull();
    InputFrameInfo.timeStamp = work->input.ordinal.timestamp.peekull();
    Result = mAmlVencInst->PreProcess(inputBuffer,InputFrameInfo);
    if (FAIL == Result) {
        return;
    }
    else if(WORK_IS_VALID == Result) {
        work->workletsProcessed = 1u;
        WorkDone(work);
        return;
    }

    if (!mOutputBlockPool) {
        std::shared_ptr<C2BlockPool> blockPool;
        c2_status_t err = GetCodec2BlockPool(C2BlockPool::BASIC_LINEAR, shared_from_this(), &blockPool);
        C2Venc_LOG(CODEC2_VENC_LOG_INFO,"Using output block pool with poolID %llu => got %llu - %d",
                (unsigned long long)C2BlockPool::BASIC_LINEAR,
                (unsigned long long)(
                        blockPool ? blockPool->getLocalId() : 111000111),
                err);
        if (err == C2_OK) {
            mOutputBlockPool = std::make_shared<BlockingBlockPool>(blockPool);
        }
        else {
            C2Venc_LOG(CODEC2_VENC_LOG_ERR,"GetCodec2BlockPool err:%d",err);
            //std::shared_ptr<C2Component::Listener> listener = state->mListener;
            mListener->onError_nb(shared_from_this(), err);
        }
    }

    if (!mOutBlock) {
        C2MemoryUsage usage = {C2MemoryUsage::CPU_READ,
                               C2MemoryUsage::CPU_WRITE};
        // TODO: error handling, proper usage, etc.
        c2_status_t err =
            mOutputBlockPool->fetchLinearBlock(mOutBufferSize, usage, &mOutBlock);
        if (err != C2_OK) {
            C2Venc_LOG(CODEC2_VENC_LOG_ERR,"fetch linear block err = %d", err);
            work->result = err;
            work->workletsProcessed = 1u;
            WorkDone(work);
            return;
        }
    }
    C2WriteView wView = mOutBlock->map().get();
    if (wView.error() != C2_OK) {
        C2Venc_LOG(CODEC2_VENC_LOG_ERR,"write view map err = %d", wView.error());
        work->result = wView.error();
        work->workletsProcessed = 1u;
        WorkDone(work);
        return;
    }

    if (!mSpsPpsHeaderReceived) {
        uint32_t uHeaderLength = 0;
        uint8_t header[128] = {0};
        bool error = mAmlVencInst->GenerateHeader((char *)header,uHeaderLength);
        if (!error) {
            C2Venc_LOG(CODEC2_VENC_LOG_ERR,"Encode header failed = 0x%x\n",error);
            work->workletsProcessed = 1u;
            WorkDone(work);
            return;
        } else {
            C2Venc_LOG(CODEC2_VENC_LOG_INFO,"Bytes Generated in header %d\n",uHeaderLength);
        }

        mSpsPpsHeaderReceived = true;

        std::unique_ptr<C2StreamInitDataInfo::output> csd = C2StreamInitDataInfo::output::AllocUnique(uHeaderLength, 0u);
        if (!csd) {
            C2Venc_LOG(CODEC2_VENC_LOG_ERR,"CSD allocation failed");
            //mSignalledError = true;
            work->result = C2_NO_MEMORY;
            work->workletsProcessed = 1u;
            WorkDone(work);
            return;
        }
        memcpy(csd->m.value, header, uHeaderLength);
        work->worklets.front()->output.configUpdate.push_back(std::move(csd));
        if (work->input.buffers.empty()) {
            work->workletsProcessed = 1u;
            C2Venc_LOG(CODEC2_VENC_LOG_ERR,"generate header already,but input buffer queue is empty");
            WorkDone(work);
            return;
        }

    }
    stOutputFrame OutInfo;
    memset(&OutInfo,0,sizeof(OutInfo));
    OutInfo.Data = wView.base();
    OutInfo.Length = wView.capacity();
    c2_status_t res = mAmlVencInst->ProcessOneFrame(InputFrameInfo,OutInfo);
    if (C2_OK == res) {
        C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"processoneframe ok,do finishwork begin!");
        finishWork(InputFrameInfo.frameIndex,work,OutInfo);
    }
}

void C2VencComp::WorkDone(std::unique_ptr<C2Work> &work) {
     if (work->workletsProcessed != 0u) {

        std::unique_ptr<C2Work> DoneWork = std::move(work);
        std::list<std::unique_ptr<C2Work>> finishedWorks;
        finishedWorks.emplace_back(std::move(DoneWork));
        mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
    }
}

void C2VencComp::ConfigParam(std::unique_ptr<C2Work> &work) {
    C2AndroidStreamAverageBlockQuantizationInfo::output mAverageBlockQuantization(0u,0);
    C2StreamPictureTypeInfo::output mPictureType(0u,C2Config::SYNC_FRAME);
    c2_status_t err = mIntf->query_vb({&mAverageBlockQuantization,&mPictureType},{},C2_DONT_BLOCK,nullptr);
    if (err == C2_OK) {
        work->worklets.front()->output.configUpdate.push_back(
                C2Param::Copy(mAverageBlockQuantization));
        work->worklets.front()->output.configUpdate.push_back(
                C2Param::Copy(mPictureType));
        }
    else {
        ALOGE("Cannot set avg_qp");
        return;
    }
}

void C2VencComp::finishWork(uint64_t workIndex, std::unique_ptr<C2Work> &work,
                              stOutputFrame OutFrameInfo) {
    std::shared_ptr<C2Buffer> buffer = createLinearBuffer(mOutBlock, 0, OutFrameInfo.Length);
    if (IDR_FRAME == OutFrameInfo.FrameType) {
        C2Venc_LOG(CODEC2_VENC_LOG_INFO,"IDR frame produced");
        buffer->setInfo(std::make_shared<C2StreamPictureTypeMaskInfo::output>(0u /* stream id */, C2Config::SYNC_FRAME));
    }
    ConfigParam(work);
    mOutBlock = nullptr;
    auto fillWork = [buffer,this](std::unique_ptr<C2Work> &work) {
        work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
        if (mSawInputEOS) {
            work->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;
        }
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.buffers.push_back(buffer);
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
    };
    fillWork(work);
    //std::list<std::unique_ptr<C2Work>> workItems
    std::unique_ptr<C2Work> DoneWork = std::move(work);
    std::list<std::unique_ptr<C2Work>> finishedWorks;
    finishedWorks.emplace_back(std::move(DoneWork));
    mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
    C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"finish this work,index:%" PRId64"",workIndex);
}

void C2VencComp::finish(
        uint64_t frameIndex, std::function<void(std::unique_ptr<C2Work> &)> fillWork) {
    std::unique_ptr<C2Work> work;
    if (work) {
        //fillWork(work);
        //std::shared_ptr<C2Component::Listener> listener = mExecState.lock()->mListener;
        /*
         * This is the logic, no need to modify, ignore coverity weak cryptor report.
        */
        /*coverity[dead_error_begin:SUPPRESS]*/
        std::unique_ptr<C2Work> DoneWork = std::move(work);
        std::list<std::unique_ptr<C2Work>> finishedWorks;
        finishedWorks.emplace_back(std::move(DoneWork));
        mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
        C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"returning pending work");
    }
}



// static
void *C2VencComp::runWorkLoop(void *arg) {
    C2VencComp *threadloop = static_cast<C2VencComp *>(arg);
    return threadloop->threadLoop();
}

void *C2VencComp::threadLoop() {
    while (!mthread.exitRequested()) {
        ProcessData();

        usleep(100);
    }
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"threadLoop exit done!");
    AutoMutex l(mProcessDoneLock);
    mProcessDoneCond.signal();
    return NULL;
}

std::shared_ptr<C2Buffer> C2VencComp::createLinearBuffer(
        const std::shared_ptr<C2LinearBlock> &block, size_t offset, size_t size) {
    return C2Buffer::CreateLinearBuffer(block->share(offset, size, ::C2Fence()));
}

std::shared_ptr<C2Buffer> C2VencComp::createGraphicBuffer(
        const std::shared_ptr<C2GraphicBlock> &block, const C2Rect &crop) {
    return C2Buffer::CreateGraphicBuffer(block->share(crop, ::C2Fence()));
}



class C2VencH264Factory : public C2ComponentFactory {
public:
    C2VencH264Factory() : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
        GetCodec2VendorComponentStore()->getParamReflector())) {
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        UNUSED(deleter);
        ALOGV("in %s", __func__);
        *component = C2VencComp::create((char *)COMPONENT_NAME_AVC, id, std::make_shared<C2VencComp::IntfImpl>(COMPONENT_NAME_AVC,MEDIA_MIMETYPE_VIDEO_AVC,mHelper));
        return *component ? C2_OK : C2_NO_MEMORY;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id,
            std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        ALOGV("in %s", __func__);
        UNUSED(deleter);
        *interface = std::shared_ptr<C2ComponentInterface>(
                new SimpleInterface<C2VencComp::IntfImpl>(
                        COMPONENT_NAME_AVC, id, std::make_shared<C2VencComp::IntfImpl>(COMPONENT_NAME_AVC,MEDIA_MIMETYPE_VIDEO_AVC,mHelper)));
        return C2_OK;
    }

    virtual ~C2VencH264Factory() override = default;

private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
};

class C2VencH265Factory : public C2ComponentFactory {
public:
    C2VencH265Factory() : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
        GetCodec2VendorComponentStore()->getParamReflector())) {
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        UNUSED(deleter);
        ALOGV("in %s", __func__);
        *component = C2VencComp::create((char *)COMPONENT_NAME_HEVC, id, std::make_shared<C2VencComp::IntfImpl>(COMPONENT_NAME_HEVC,MEDIA_MIMETYPE_VIDEO_HEVC,mHelper));
        return *component ? C2_OK : C2_NO_MEMORY;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id,
            std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        ALOGV("in %s", __func__);
        UNUSED(deleter);
        *interface = std::shared_ptr<C2ComponentInterface>(
                new SimpleInterface<C2VencComp::IntfImpl>(
                        COMPONENT_NAME_HEVC, id, std::make_shared<C2VencComp::IntfImpl>(COMPONENT_NAME_HEVC,MEDIA_MIMETYPE_VIDEO_HEVC,mHelper)));
        return C2_OK;
    }

    virtual ~C2VencH265Factory() override = default;

private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
};


}


__attribute__((cfi_canonical_jump_table))
extern "C" ::C2ComponentFactory* CreateC2VencH264Factory() {
    ALOGV("in %s", __func__);
    return new ::android::C2VencH264Factory();
}

__attribute__((cfi_canonical_jump_table))
extern "C" void DestroyC2VencH264Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}

__attribute__((cfi_canonical_jump_table))
extern "C" ::C2ComponentFactory* CreateC2VencH265Factory() {
    ALOGV("in %s", __func__);
    return new ::android::C2VencH265Factory();
}

__attribute__((cfi_canonical_jump_table))
extern "C" void DestroyC2VencH265Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}



