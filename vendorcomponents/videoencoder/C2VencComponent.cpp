// Copyright 2019. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define LOG_NDEBUG 0
#define LOG_TAG "C2VencComponent"

#include <C2VencComponent.h>
#include <C2AllocatorGralloc.h>
#include <C2ComponentFactory.h>
#include <C2PlatformSupport.h>

#include <cutils/native_handle.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AUtils.h>


#include <ui/GraphicBuffer.h>
#include <utils/Log.h>
#include <cutils/properties.h>

#include <string>
#include <inttypes.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <stdio.h>

namespace android {

#define OUTPUT_BUFFERSIZE_MIN (2 * 1024 * 1024)
#define ENABLE_DUMP_ES        1
#define ENABLE_DUMP_RAW       (1 << 1)
#define ENCODER_PROP_DUMP_DATA        "debug.vendor.media.c2.venc.dump_data"

#define C2Venc_LOG(level, fmt, str...) CODEC2_LOG(level, fmt, ##str)

class C2VencComponent::BlockingBlockPool : public C2BlockPool {
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


bool C2VencComponent::IsYUV420(const C2GraphicView &view) {
    const C2PlanarLayout &layout = view.layout();
    return (layout.numPlanes == 3
            && layout.type == C2PlanarLayout::TYPE_YUV
            && layout.planes[layout.PLANE_Y].channel == C2PlaneInfo::CHANNEL_Y
            && layout.planes[layout.PLANE_Y].allocatedDepth == 8
            && layout.planes[layout.PLANE_Y].bitDepth == 8
            && layout.planes[layout.PLANE_Y].rightShift == 0
            && layout.planes[layout.PLANE_Y].colSampling == 1
            && layout.planes[layout.PLANE_Y].rowSampling == 1
            && layout.planes[layout.PLANE_U].channel == C2PlaneInfo::CHANNEL_CB
            && layout.planes[layout.PLANE_U].allocatedDepth == 8
            && layout.planes[layout.PLANE_U].bitDepth == 8
            && layout.planes[layout.PLANE_U].rightShift == 0
            && layout.planes[layout.PLANE_U].colSampling == 2
            && layout.planes[layout.PLANE_U].rowSampling == 2
            && layout.planes[layout.PLANE_V].channel == C2PlaneInfo::CHANNEL_CR
            && layout.planes[layout.PLANE_V].allocatedDepth == 8
            && layout.planes[layout.PLANE_V].bitDepth == 8
            && layout.planes[layout.PLANE_V].rightShift == 0
            && layout.planes[layout.PLANE_V].colSampling == 2
            && layout.planes[layout.PLANE_V].rowSampling == 2);
}

bool C2VencComponent::IsNV12(const C2GraphicView &view) {
    if (!IsYUV420(view)) {
        return false;
    }
    const C2PlanarLayout &layout = view.layout();
    return (layout.rootPlanes == 2
            && layout.planes[layout.PLANE_U].colInc == 2
            && layout.planes[layout.PLANE_U].rootIx == layout.PLANE_U
            && layout.planes[layout.PLANE_U].offset == 0
            && layout.planes[layout.PLANE_V].colInc == 2
            && layout.planes[layout.PLANE_V].rootIx == layout.PLANE_U
            && layout.planes[layout.PLANE_V].offset == 1);
}

bool C2VencComponent::IsNV21(const C2GraphicView &view) {
    if (!IsYUV420(view)) {
        return false;
    }
    const C2PlanarLayout &layout = view.layout();
    return (layout.rootPlanes == 2
            && layout.planes[layout.PLANE_U].colInc == 2
            && layout.planes[layout.PLANE_U].rootIx == layout.PLANE_V
            && layout.planes[layout.PLANE_U].offset == 1
            && layout.planes[layout.PLANE_V].colInc == 2
            && layout.planes[layout.PLANE_V].rootIx == layout.PLANE_V
            && layout.planes[layout.PLANE_V].offset == 0);
}

bool C2VencComponent::IsI420(const C2GraphicView &view) {
    if (!IsYUV420(view)) {
        return false;
    }
    const C2PlanarLayout &layout = view.layout();
    return (layout.rootPlanes == 3
            && layout.planes[layout.PLANE_U].colInc == 1
            && layout.planes[layout.PLANE_U].rootIx == layout.PLANE_U
            && layout.planes[layout.PLANE_U].offset == 0
            && layout.planes[layout.PLANE_V].colInc == 1
            && layout.planes[layout.PLANE_V].rootIx == layout.PLANE_V
            && layout.planes[layout.PLANE_V].offset == 0);
}



C2VencComponent::C2VencComponent(const std::shared_ptr<C2ComponentInterface> &intf)
                : mComponentState(ComponentState::UNINITIALIZED),
                  mIntf(intf),
                  mIsInit(false),
                  mfdDumpInput(-1),
                  mfdDumpOutput(-1),
                  mSpsPpsHeaderReceived(false),
                  mOutBufferSize(OUTPUT_BUFFERSIZE_MIN),
                  mSawInputEOS(false),
                  mDumpYuvEnable(false),
                  mDumpEsEnable(false) {
    ALOGD("C2VencComponent constructor!");
    propGetInt(CODEC2_VENC_LOGDEBUG_PROPERTY, &gloglevel);
    ALOGD("gloglevel:%x",gloglevel);
}

C2VencComponent::~C2VencComponent() {
    ALOGD("C2VencComponent destructor!");
}

c2_status_t C2VencComponent::setListener_vb(const std::shared_ptr<Listener>& listener,
                                           c2_blocking_t mayBlock) {
    UNUSED(mayBlock);
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"C2VencComponent setListener_vb!");
    mListener = listener;
    return C2_OK;
}

c2_status_t C2VencComponent::queue_nb(std::list<std::unique_ptr<C2Work>>* const items) {
    ALOGD("C2VencComponent queue_nb!,receive buffer count:%d", (int)items->size());
    AutoMutex l(mInputQueueLock);
    while (!items->empty()) {
        mQueue.push_back(std::move(items->front()));
        items->pop_front();
    }
    return C2_OK;
}

c2_status_t C2VencComponent::announce_nb(const std::vector<C2WorkOutline> &items) {
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"C2VencComponent announce_nb!");

    return C2_OK;
}

c2_status_t C2VencComponent::flush_sm(flush_mode_t mode, std::list<std::unique_ptr<C2Work>>* const flushedWork) {
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

c2_status_t C2VencComponent::drain_nb(drain_mode_t mode) {
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"C2VencComponent drain_nb!");

    return C2_OK;
}

c2_status_t C2VencComponent::start() {
    std::string strFileName;
    uint32_t isDumpFile = 0;
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

    isDumpFile = property_get_int32(ENCODER_PROP_DUMP_DATA, 0);
    if (isDumpFile & ENABLE_DUMP_RAW) {
        mDumpYuvEnable = true;
        getCodecDumpFileName(strFileName,C2_DUMP_RAW);
        C2Venc_LOG(CODEC2_VENC_LOG_INFO,"Enable Dump yuv file, name: %s", strFileName.c_str());
        mfdDumpInput = open(strFileName.c_str(), O_CREAT | O_LARGEFILE | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
        if (mfdDumpInput < 0) {
            C2Venc_LOG(CODEC2_VENC_LOG_ERR,"Dump Input File handle error!");
        }
    }

    if (isDumpFile & ENABLE_DUMP_ES) {
        mDumpEsEnable = true;
        getCodecDumpFileName(strFileName,C2_DUMP_ES);
        C2Venc_LOG(CODEC2_VENC_LOG_INFO,"Enable Dump es file, name: %s", strFileName.c_str());
        mfdDumpOutput = open(strFileName.c_str(), O_CREAT | O_LARGEFILE | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
        if (mfdDumpOutput < 0) {
            C2Venc_LOG(CODEC2_VENC_LOG_ERR,"Dump Output File handle error!");
        }
    }
    return C2_OK;
}

c2_status_t C2VencComponent::stop() {
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
    }
    if (mfdDumpInput >= 0) {
        close(mfdDumpInput);
        mfdDumpInput = -1;
        C2Venc_LOG(CODEC2_VENC_LOG_INFO,"Dump raw File finish!");
    }

    if (mfdDumpOutput >= 0) {
        close(mfdDumpOutput);
        mfdDumpOutput = -1;
        C2Venc_LOG(CODEC2_VENC_LOG_INFO,"Dump raw File finish!");
    }
    Close();
    mComponentState = ComponentState::UNINITIALIZED;
    return C2_OK;
}

c2_status_t C2VencComponent::reset() {
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"C2VencComponent reset!");
    stop();
    if (mOutBlock) {
        mOutBlock.reset();
    }
    //Init(); //we will do init function in start process
    return C2_OK;
}

c2_status_t C2VencComponent::release() {
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"C2VencComponent release!");
    stop();
    return C2_OK;
}

std::shared_ptr<C2ComponentInterface> C2VencComponent::intf() {
    return mIntf;
}

bool C2VencComponent::doSomeInit() {
    if (!mIsInit) {
        C2Venc_LOG(CODEC2_VENC_LOG_INFO,"now init encoder module");
        if (!LoadModule())
            return false;
        if (C2_OK != Init())
            return false;
    }
    return true;
}


bool C2VencComponent::OpenFile(int *fd,char *pName) {
    C2Venc_LOG(CODEC2_VENC_LOG_INFO,"open dump file, name: %s", pName);
    (*fd) = open(pName, O_CREAT | O_LARGEFILE | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
    if (*fd < 0) {
        C2Venc_LOG(CODEC2_VENC_LOG_ERR,"Dump Input File handle error!");
        return false;
    }
    return true;
}



uint32_t C2VencComponent::dumpDataToFile(int fd,uint8_t *data,uint32_t size) {
    uint32_t uWriteLen = 0;
    if (fd >= 0 && (data != 0)) {
        C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"dump data size: %d",size);
        uWriteLen = write(fd, (unsigned char *)data, size);
    }
    return uWriteLen;
}


void C2VencComponent::ProcessData()
{
    uint32_t dumpFileSize = 0;
    std::unique_ptr<C2Work> work;
    InputFrameInfo_t InputFrameInfo;
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

    std::shared_ptr<const C2GraphicView> view;
    std::shared_ptr<C2Buffer> inputBuffer;
    if (!work->input.buffers.empty()) {
        inputBuffer = work->input.buffers[0];
        view = std::make_shared<const C2GraphicView>(
                inputBuffer->data().graphicBlocks().front().map().get());
        if (view->error() != C2_OK) {
            C2Venc_LOG(CODEC2_VENC_LOG_ERR,"graphic view map err = %d", view->error());
            work->workletsProcessed = 1u;
            WorkDone(work);
            return;
        }
    }

    //C2Handle *handle = inputBuffer->data.graphicBlocks().front().handle();

    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;

    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        C2Venc_LOG(CODEC2_VENC_LOG_ERR,"saw eos flag");
        mSawInputEOS = true;
    }

    if (view.get() == nullptr) {
        C2Venc_LOG(CODEC2_VENC_LOG_ERR,"graphic view is null");
        work->workletsProcessed = 1u;
        WorkDone(work);
        return;
    }

    /*if (view.get()->width() < mSize->width ||
        view.get()->height() < mSize->height) {
        ALOGW("unexpected Capacity Aspect %d(%d) x %d(%d)", view.get()->width(),
              mSize->width, view.get()->height(), mSize->height);
        return;
    }*/

    //C2StreamPictureSizeInfo::input size;
    //std::vector<std::unique_ptr<C2Param>> queried;
    //c2_status_t c2err = intf()->query_vb({ &size}, {}, C2_DONT_BLOCK, &queried);


    const C2PlanarLayout &layout = view->layout();
    InputFrameInfo.yPlane = const_cast<uint8_t *>(view->data()[C2PlanarLayout::PLANE_Y]);
    InputFrameInfo.uPlane = const_cast<uint8_t *>(view->data()[C2PlanarLayout::PLANE_U]);
    InputFrameInfo.vPlane = const_cast<uint8_t *>(view->data()[C2PlanarLayout::PLANE_V]);
    InputFrameInfo.yStride = layout.planes[C2PlanarLayout::PLANE_Y].rowInc;
    InputFrameInfo.uStride = layout.planes[C2PlanarLayout::PLANE_U].rowInc;
    InputFrameInfo.vStride = layout.planes[C2PlanarLayout::PLANE_V].rowInc;
    dumpFileSize = view->width() * view->height() * 3 / 2;
    C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"yStride:%d,uStride:%d,vStride:%d,view->width():%d,view->height():%d,root plane num:%d,component plan num:%d",
        InputFrameInfo.yStride,InputFrameInfo.uStride,InputFrameInfo.vStride,view->width(),view->height(),layout.rootPlanes,layout.numPlanes);
    switch (layout.type) {
        case C2PlanarLayout::TYPE_RGB:
        case C2PlanarLayout::TYPE_RGBA: {
            C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"TYPE_RGBA or TYPE_RGB");
            InputFrameInfo.yPlane = const_cast<uint8_t *>(view->data()[C2PlanarLayout::PLANE_R]);
            InputFrameInfo.uPlane = const_cast<uint8_t *>(view->data()[C2PlanarLayout::PLANE_G]);
            InputFrameInfo.vPlane = const_cast<uint8_t *>(view->data()[C2PlanarLayout::PLANE_B]);
            InputFrameInfo.yStride = layout.planes[C2PlanarLayout::PLANE_R].rowInc;
            InputFrameInfo.uStride = layout.planes[C2PlanarLayout::PLANE_G].rowInc;
            InputFrameInfo.vStride = layout.planes[C2PlanarLayout::PLANE_B].rowInc;
            dumpFileSize = view->width() * view->height() * 4;
            InputFrameInfo.colorFmt = C2_ENC_FMT_RGBA8888;
            break;
        }
        case C2PlanarLayout::TYPE_YUV: {
            C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"TYPE_YUV");
            if (IsNV12(*view.get())) {
                InputFrameInfo.colorFmt = C2_ENC_FMT_NV12;
                C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"InputFrameInfo colorfmt :C2_ENC_FMT_NV12");
            }
            else if (IsNV21(*view.get())) {
                InputFrameInfo.colorFmt = C2_ENC_FMT_NV21;
                C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"InputFrameInfo colorfmt :C2_ENC_FMT_NV21");
            }
            else if (IsI420(*view.get())) {
                InputFrameInfo.colorFmt = C2_ENC_FMT_I420;
                C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"InputFrameInfo colorfmt :C2_ENC_FMT_I420");
            }
            else {
                C2Venc_LOG(CODEC2_VENC_LOG_ERR,"type yuv,but not support fmt!!!");
            }
            dumpFileSize = InputFrameInfo.yStride * view->height() * 3 / 2;
            break;
        }
        case C2PlanarLayout::TYPE_YUVA: {
            C2Venc_LOG(CODEC2_VENC_LOG_ERR,"TYPE_YUVA not support!!");
            break;
        }
        default:
            C2Venc_LOG(CODEC2_VENC_LOG_ERR,"layout.type:%d",layout.type);
            break;
    }
    //InputFrameInfo.colorFmt = layout.type;
    if (mDumpYuvEnable) {
        dumpDataToFile(mfdDumpInput,InputFrameInfo.yPlane,dumpFileSize);
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
        c2_status_t error = GenerateHeader(header,&uHeaderLength);
        if (C2_OK != error) {
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
        if (mDumpEsEnable) {
            dumpDataToFile(mfdDumpOutput,header,uHeaderLength);
        }
        if (work->input.buffers.empty()) {
            work->workletsProcessed = 1u;
            C2Venc_LOG(CODEC2_VENC_LOG_ERR,"generate header already,but input buffer queue is empty");
            WorkDone(work);
            return;
        }
    }
    OutputFrameInfo_t OutInfo;
    memset(&OutInfo,0,sizeof(OutInfo));
    OutInfo.Data = wView.base();
    OutInfo.Length = wView.capacity();
    InputFrameInfo.frameIndex = work->input.ordinal.frameIndex.peekull();
    InputFrameInfo.timeStamp = work->input.ordinal.timestamp.peekull();
    c2_status_t res = ProcessOneFrame(InputFrameInfo,&OutInfo);
    if (C2_OK == res) {
        if (mDumpEsEnable) {
            dumpDataToFile(mfdDumpOutput,OutInfo.Data,OutInfo.Length);
        }
        C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"processoneframe ok,do finishwork begin!");
        finishWork(InputFrameInfo.frameIndex,work,OutInfo);
    }
}

void C2VencComponent::WorkDone(std::unique_ptr<C2Work> &work) {
     if (work->workletsProcessed != 0u) {

        std::unique_ptr<C2Work> DoneWork = std::move(work);
        std::list<std::unique_ptr<C2Work>> finishedWorks;
        finishedWorks.emplace_back(std::move(DoneWork));
        mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
    }
}


void C2VencComponent::finishWork(uint64_t workIndex, std::unique_ptr<C2Work> &work,
                              OutputFrameInfo_t OutFrameInfo) {
    std::shared_ptr<C2Buffer> buffer = createLinearBuffer(mOutBlock, 0, OutFrameInfo.Length);
    if (FRAMETYPE_IDR == OutFrameInfo.FrameType) {
        C2Venc_LOG(CODEC2_VENC_LOG_INFO,"IDR frame produced");
        buffer->setInfo(std::make_shared<C2StreamPictureTypeMaskInfo::output>(0u /* stream id */, C2Config::SYNC_FRAME));
    }
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
    ALOGI("finish this work,index:%" PRId64"",workIndex);
    #if 0
    if (work && c2_cntr64_t(workIndex) == work->input.ordinal.frameIndex) {
        fillWork(work);
        mListener->onWorkDone_nb(shared_from_this(), vec(work));
        //if (mSawInputEOS) {
        //    work->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;
        //}
    } else {
        finish(workIndex, fillWork);
    }
    #endif
}

void C2VencComponent::finish(
        uint64_t frameIndex, std::function<void(std::unique_ptr<C2Work> &)> fillWork) {
    std::unique_ptr<C2Work> work;
    {
    #if 0
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        if (mQueue->pending().count(frameIndex) == 0) {
            ALOGW("unknown frame index: %" PRIu64, frameIndex);
            return;
        }
        work = std::move(mQueue->pending().at(frameIndex));
        mQueue->pending().erase(frameIndex);
    #endif
    }
    if (work) {
        //fillWork(work);
        //std::shared_ptr<C2Component::Listener> listener = mExecState.lock()->mListener;
        std::unique_ptr<C2Work> DoneWork = std::move(work);
        std::list<std::unique_ptr<C2Work>> finishedWorks;
        finishedWorks.emplace_back(std::move(DoneWork));
        mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
        C2Venc_LOG(CODEC2_VENC_LOG_DEBUG,"returning pending work");
    }
}



// static
void *C2VencComponent::runWorkLoop(void *arg) {
    C2VencComponent *threadloop = static_cast<C2VencComponent *>(arg);
    return threadloop->threadLoop();
}

void *C2VencComponent::threadLoop() {
    while (!mthread.exitRequested()) {
        ProcessData();
        /*{
            if (isVideoDecorder()) {
                AutoMutex l(mInputQueueLock);
                if (mInputQueue.empty()) {
                    mInputQueueCond.waitRelative(mInputQueueLock, 16000000);
                }
            }
            AutoMutex l(mLoopLock);
            // The processing should stop when paused.
            if (!mPaused) {
                if (!isInputQueueEmpty())
                    doWriteBuffer();
                if (!isVideoDecorder())
                    doReadBuffer();
            }
        }
        if (!isVideoDecorder())
            usleep(500);  // sleep for 0.5 millisecond*/
        usleep(100);
    }
    return NULL;
}

std::shared_ptr<C2Buffer> C2VencComponent::createLinearBuffer(
        const std::shared_ptr<C2LinearBlock> &block, size_t offset, size_t size) {
    return C2Buffer::CreateLinearBuffer(block->share(offset, size, ::C2Fence()));
}

std::shared_ptr<C2Buffer> C2VencComponent::createGraphicBuffer(
        const std::shared_ptr<C2GraphicBlock> &block, const C2Rect &crop) {
    return C2Buffer::CreateGraphicBuffer(block->share(crop, ::C2Fence()));
}


}



