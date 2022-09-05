/*
 * Copyright (C) 2017 The Android Open Source Project
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
#define LOG_TAG "Amlogic_C2AudioDecComponent"
#include <log/log.h>

#include <android/hardware_buffer.h>
#include <cutils/properties.h>
#include <media/stagefright/foundation/AMessage.h>

#include <inttypes.h>

#include <C2Config.h>
#include <C2Debug.h>
#include <C2PlatformSupport.h>
#include <Codec2BufferUtils.h>
#include <Codec2CommonUtils.h>
#include <C2AudioDecComponent.h>

namespace android {

std::unique_ptr<C2Work> C2AudioDecComponent::WorkQueue::pop_front() {
    std::unique_ptr<C2Work> work = std::move(mQueue.front().work);
    mQueue.pop_front();
    return work;
}

void C2AudioDecComponent::WorkQueue::push_back(std::unique_ptr<C2Work> work) {
    mQueue.push_back({ std::move(work), NO_DRAIN });
}

bool C2AudioDecComponent::WorkQueue::empty() const {
    return mQueue.empty();
}

void C2AudioDecComponent::WorkQueue::clear() {
    mQueue.clear();
}

uint32_t C2AudioDecComponent::WorkQueue::drainMode() const {
    return mQueue.front().drainMode;
}

void C2AudioDecComponent::WorkQueue::markDrain(uint32_t drainMode) {
    mQueue.push_back({ nullptr, drainMode });
}

////////////////////////////////////////////////////////////////////////////////

C2AudioDecComponent::WorkHandler::WorkHandler() : mRunning(false) {}

void C2AudioDecComponent::WorkHandler::setComponent(
        const std::shared_ptr<C2AudioDecComponent> &thiz) {
    mThiz = thiz;
}

static void Reply(const sp<AMessage> &msg, int32_t *err = nullptr) {
    sp<AReplyToken> replyId;
    CHECK(msg->senderAwaitsResponse(&replyId));
    sp<AMessage> reply = new AMessage;
    if (err) {
        reply->setInt32("err", *err);
    }
    reply->postReply(replyId);
}

void C2AudioDecComponent::WorkHandler::onMessageReceived(const sp<AMessage> &msg) {
    std::shared_ptr<C2AudioDecComponent> thiz = mThiz.lock();
    if (!thiz) {
        ALOGD("component not yet set; msg = %s", msg->debugString().c_str());
        sp<AReplyToken> replyId;
        if (msg->senderAwaitsResponse(&replyId)) {
            sp<AMessage> reply = new AMessage;
            reply->setInt32("err", C2_CORRUPTED);
            reply->postReply(replyId);
        }
        return;
    }

    switch (msg->what()) {
        case kWhatProcess: {
            if (mRunning) {
                if (thiz->processQueue()) {
                    (new AMessage(kWhatProcess, this))->post();
                }
            } else {
                ALOGV("Ignore process message as we're not running");
            }
            break;
        }
        case kWhatInit: {
            int32_t err = thiz->onInit();
            Reply(msg, &err);
            [[fallthrough]];
        }
        case kWhatStart: {
            mRunning = true;
            break;
        }
        case kWhatStop: {
            int32_t err = thiz->onStop();
            thiz->mOutputBlockPool.reset();
            Reply(msg, &err);
            break;
        }
        case kWhatReset: {
            thiz->onReset();
            thiz->mOutputBlockPool.reset();
            mRunning = false;
            Reply(msg);
            break;
        }
        case kWhatRelease: {
            thiz->onRelease();
            thiz->mOutputBlockPool.reset();
            mRunning = false;
            Reply(msg);
            break;
        }
        default: {
            ALOGD("Unrecognized msg: %d", msg->what());
            break;
        }
    }
}

class C2AudioDecComponent::BlockingBlockPool : public C2BlockPool {
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

////////////////////////////////////////////////////////////////////////////////

namespace {

struct DummyReadView : public C2ReadView {
    DummyReadView() : C2ReadView(C2_NO_INIT) {}
};

}  // namespace

C2AudioDecComponent::C2AudioDecComponent(
        const std::shared_ptr<C2ComponentInterface> &intf)
    : mDummyReadView(DummyReadView()),
      mIntf(intf),
      mLooper(new ALooper),
      mHandler(new WorkHandler) {
    mLooper->setName(intf->getName().c_str());
    (void)mLooper->registerHandler(mHandler);
    mLooper->start(false, false, ANDROID_PRIORITY_VIDEO);
}

C2AudioDecComponent::~C2AudioDecComponent() {
    mLooper->unregisterHandler(mHandler->id());
    (void)mLooper->stop();
}

c2_status_t C2AudioDecComponent::setListener_vb(
        const std::shared_ptr<C2Component::Listener> &listener, c2_blocking_t mayBlock) {
    mHandler->setComponent(shared_from_this());

    Mutexed<ExecState>::Locked state(mExecState);
    if (state->mState == RUNNING) {
        if (listener) {
            return C2_BAD_STATE;
        } else if (!mayBlock) {
            return C2_BLOCKING;
        }
    }
    state->mListener = listener;
    // TODO: wait for listener change to have taken place before returning
    // (e.g. if there is an ongoing listener callback)
    return C2_OK;
}

c2_status_t C2AudioDecComponent::queue_nb(std::list<std::unique_ptr<C2Work>> * const items) {
    ALOGV("%s() %d", __func__, __LINE__);
    {
        Mutexed<ExecState>::Locked state(mExecState);
        if (state->mState != RUNNING) {
            return C2_BAD_STATE;
        }
    }
    bool queueWasEmpty = false;
    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        queueWasEmpty = queue->empty();
        while (!items->empty()) {
            queue->push_back(std::move(items->front()));
            items->pop_front();
        }
    }
    if (queueWasEmpty) {
        (new AMessage(WorkHandler::kWhatProcess, mHandler))->post();
    }
    return C2_OK;
}

c2_status_t C2AudioDecComponent::announce_nb(const std::vector<C2WorkOutline> &items) {
    (void)items;
    return C2_OMITTED;
}

c2_status_t C2AudioDecComponent::flush_sm(
        flush_mode_t flushMode, std::list<std::unique_ptr<C2Work>>* const flushedWork) {
        ALOGV("%s() %d", __func__, __LINE__);
    (void)flushMode;
    {
        Mutexed<ExecState>::Locked state(mExecState);
        if (state->mState != RUNNING) {
            return C2_BAD_STATE;
        }
    }
    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        queue->incGeneration();
        // TODO: queue->splicedBy(flushedWork, flushedWork->end());
        while (!queue->empty()) {
            std::unique_ptr<C2Work> work = queue->pop_front();
            if (work) {
                flushedWork->push_back(std::move(work));
            }
        }
        while (!queue->pending().empty()) {
            flushedWork->push_back(std::move(queue->pending().begin()->second));
            queue->pending().erase(queue->pending().begin());
        }
    }

    return C2_OK;
}

c2_status_t C2AudioDecComponent::drain_nb(drain_mode_t drainMode) {
    ALOGV("%s() %d", __func__, __LINE__);
    if (drainMode == DRAIN_CHAIN) {
        return C2_OMITTED;
    }
    {
        Mutexed<ExecState>::Locked state(mExecState);
        if (state->mState != RUNNING) {
            return C2_BAD_STATE;
        }
    }
    bool queueWasEmpty = false;
    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        queueWasEmpty = queue->empty();
        queue->markDrain(drainMode);
    }
    if (queueWasEmpty) {
        (new AMessage(WorkHandler::kWhatProcess, mHandler))->post();
    }

    return C2_OK;
}

c2_status_t C2AudioDecComponent::start() {
    ALOGV("%s() %d", __func__, __LINE__);
    Mutexed<ExecState>::Locked state(mExecState);
    if (state->mState == RUNNING) {
        return C2_BAD_STATE;
    }
    bool needsInit = (state->mState == UNINITIALIZED);
    state.unlock();
    if (needsInit) {
        sp<AMessage> reply;
        (new AMessage(WorkHandler::kWhatInit, mHandler))->postAndAwaitResponse(&reply);
        int32_t err;
        CHECK(reply->findInt32("err", &err));
        if (err != C2_OK) {
            return (c2_status_t)err;
        }
    } else {
        (new AMessage(WorkHandler::kWhatStart, mHandler))->post();
    }
    state.lock();
    state->mState = RUNNING;
    return C2_OK;
}

c2_status_t C2AudioDecComponent::stop() {
    ALOGV("%s() %d", __func__, __LINE__);
    {
        Mutexed<ExecState>::Locked state(mExecState);
        if (state->mState != RUNNING) {
            return C2_BAD_STATE;
        }
        state->mState = STOPPED;
    }
    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        queue->clear();
        queue->pending().clear();
    }
    sp<AMessage> reply;
    (new AMessage(WorkHandler::kWhatStop, mHandler))->postAndAwaitResponse(&reply);
    int32_t err;
    CHECK(reply->findInt32("err", &err));
    if (err != C2_OK) {
        return (c2_status_t)err;
    }
    return C2_OK;
}

c2_status_t C2AudioDecComponent::reset() {
    ALOGV("%s() %d", __func__, __LINE__);
    {
        Mutexed<ExecState>::Locked state(mExecState);
        state->mState = UNINITIALIZED;
    }
    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        queue->clear();
        queue->pending().clear();
    }
    sp<AMessage> reply;
    (new AMessage(WorkHandler::kWhatReset, mHandler))->postAndAwaitResponse(&reply);
    return C2_OK;
}

c2_status_t C2AudioDecComponent::release() {
    ALOGV("%s() %d", __func__, __LINE__);
    sp<AMessage> reply;
    (new AMessage(WorkHandler::kWhatRelease, mHandler))->postAndAwaitResponse(&reply);
    return C2_OK;
}

std::shared_ptr<C2ComponentInterface> C2AudioDecComponent::intf() {
    ALOGV("%s() %d", __func__, __LINE__);
    return mIntf;
}

namespace {

std::list<std::unique_ptr<C2Work>> vec(std::unique_ptr<C2Work> &work) {
    ALOGV("%s() %d", __func__, __LINE__);
    std::list<std::unique_ptr<C2Work>> ret;
    ret.push_back(std::move(work));
    return ret;
}

}  // namespace

void C2AudioDecComponent::finish(
        uint64_t frameIndex, std::function<void(const std::unique_ptr<C2Work> &)> fillWork) {
    std::unique_ptr<C2Work> work;
    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        if (queue->pending().count(frameIndex) == 0) {
            ALOGW("unknown frame index: %" PRIu64, frameIndex);
            return;
        }
        work = std::move(queue->pending().at(frameIndex));
        queue->pending().erase(frameIndex);
    }
    if (work) {
        fillWork(work);
        std::shared_ptr<C2Component::Listener> listener = mExecState.lock()->mListener;
        listener->onWorkDone_nb(shared_from_this(), vec(work));
        ALOGV("returning pending work");
    }
}

void C2AudioDecComponent::cloneAndSend(
        uint64_t frameIndex,
        const std::unique_ptr<C2Work> &currentWork,
        std::function<void(const std::unique_ptr<C2Work> &)> fillWork) {
    std::unique_ptr<C2Work> work(new C2Work);
    if (currentWork->input.ordinal.frameIndex == frameIndex) {
        work->input.flags = currentWork->input.flags;
        work->input.ordinal = currentWork->input.ordinal;
    } else {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        if (queue->pending().count(frameIndex) == 0) {
            ALOGW("unknown frame index: %" PRIu64, frameIndex);
            return;
        }
        work->input.flags = queue->pending().at(frameIndex)->input.flags;
        work->input.ordinal = queue->pending().at(frameIndex)->input.ordinal;
    }
    work->worklets.emplace_back(new C2Worklet);
    if (work) {
        fillWork(work);
        std::shared_ptr<C2Component::Listener> listener = mExecState.lock()->mListener;
        listener->onWorkDone_nb(shared_from_this(), vec(work));
        ALOGV("cloned and sending work");
    }
}

bool C2AudioDecComponent::processQueue() {
    std::unique_ptr<C2Work> work;
    uint64_t generation;
    int32_t drainMode;
    bool isFlushPending = false;
    bool hasQueuedWork = false;
    ALOGV("%s  ", __func__);
    {
        Mutexed<WorkQueue>::Locked queue(mWorkQueue);
        if (queue->empty()) {
            return false;
        }

        generation = queue->generation();
        drainMode = queue->drainMode();
        isFlushPending = queue->popPendingFlush();
        work = queue->pop_front();
        hasQueuedWork = !queue->empty();
    }
    if (isFlushPending) {
        ALOGV("processing pending flush");
        c2_status_t err = onFlush_sm();
        if (err != C2_OK) {
            ALOGD("flush err: %d", err);
            // TODO: error
        }
    }

    if (!mOutputBlockPool) {
        c2_status_t err = [this] {
            // TODO: don't use query_vb
            C2StreamBufferTypeSetting::output outputFormat(0u);
            std::vector<std::unique_ptr<C2Param>> params;
            c2_status_t err = intf()->query_vb(
                    { &outputFormat },
                    { C2PortBlockPoolsTuning::output::PARAM_TYPE },
                    C2_DONT_BLOCK,
                    &params);
            if (err != C2_OK && err != C2_BAD_INDEX) {
                ALOGD("query err = %d", err);
                return err;
            }
            C2BlockPool::local_id_t poolId =
                outputFormat.value == C2BufferData::GRAPHIC
                        ? C2BlockPool::BASIC_GRAPHIC
                        : C2BlockPool::BASIC_LINEAR;
            if (params.size()) {
                C2PortBlockPoolsTuning::output *outputPools =
                    C2PortBlockPoolsTuning::output::From(params[0].get());
                if (outputPools && outputPools->flexCount() >= 1) {
                    poolId = outputPools->m.values[0];
                }
            }

            std::shared_ptr<C2BlockPool> blockPool;
            err = GetCodec2BlockPool(poolId, shared_from_this(), &blockPool);
            ALOGD("Using output block pool with poolID %llu => got %llu - %d",
                    (unsigned long long)poolId,
                    (unsigned long long)(
                            blockPool ? blockPool->getLocalId() : 111000111),
                    err);
            if (err == C2_OK) {
                mOutputBlockPool = std::make_shared<BlockingBlockPool>(blockPool);
            }
            return err;
        }();
        if (err != C2_OK) {
            Mutexed<ExecState>::Locked state(mExecState);
            std::shared_ptr<C2Component::Listener> listener = state->mListener;
            state.unlock();
            listener->onError_nb(shared_from_this(), err);
            return hasQueuedWork;
        }
    }

    if (!work) {
        c2_status_t err = drain(drainMode, mOutputBlockPool);
        if (err != C2_OK) {
            Mutexed<ExecState>::Locked state(mExecState);
            std::shared_ptr<C2Component::Listener> listener = state->mListener;
            state.unlock();
            listener->onError_nb(shared_from_this(), err);
        }
        return hasQueuedWork;
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
            ALOGD("applied %zu configUpdates => %s (%d)", updates.size(), asString(err), err);
        }
    }

    ALOGV("start processing frame #%" PRIu64, work->input.ordinal.frameIndex.peeku());
    // If input buffer list is not empty, it means we have some input to process on.
    // However, input could be a null buffer. In such case, clear the buffer list
    // before making call to process().
    if (!work->input.buffers.empty() && !work->input.buffers[0]) {
        ALOGD("Encountered null input buffer. Clearing the input buffer");
        work->input.buffers.clear();
    }
    process(work, mOutputBlockPool);
    ALOGV("processed frame #%" PRIu64, work->input.ordinal.frameIndex.peeku());
    Mutexed<WorkQueue>::Locked queue(mWorkQueue);
    if (queue->generation() != generation) {
        ALOGD("work form old generation: was %" PRIu64 " now %" PRIu64,
                queue->generation(), generation);
        work->result = C2_NOT_FOUND;
        queue.unlock();

        Mutexed<ExecState>::Locked state(mExecState);
        std::shared_ptr<C2Component::Listener> listener = state->mListener;
        state.unlock();
        listener->onWorkDone_nb(shared_from_this(), vec(work));
        return hasQueuedWork;
    }
    if (work->workletsProcessed != 0u) {
        queue.unlock();
        Mutexed<ExecState>::Locked state(mExecState);
        ALOGV("returning this work");
        std::shared_ptr<C2Component::Listener> listener = state->mListener;
        state.unlock();
        listener->onWorkDone_nb(shared_from_this(), vec(work));
    } else {
        ALOGV("queue pending work");
        work->input.buffers.clear();
        std::unique_ptr<C2Work> unexpected;

        uint64_t frameIndex = work->input.ordinal.frameIndex.peeku();
        if (queue->pending().count(frameIndex) != 0) {
            unexpected = std::move(queue->pending().at(frameIndex));
            queue->pending().erase(frameIndex);
        }
        (void)queue->pending().insert({ frameIndex, std::move(work) });

        queue.unlock();
        if (unexpected) {
            ALOGD("unexpected pending work");
            unexpected->result = C2_CORRUPTED;
            Mutexed<ExecState>::Locked state(mExecState);
            std::shared_ptr<C2Component::Listener> listener = state->mListener;
            state.unlock();
            listener->onWorkDone_nb(shared_from_this(), vec(unexpected));
        }
    }
    return hasQueuedWork;
}

int C2AudioDecComponent::getHalPixelFormatForBitDepth10(bool allowRGBA1010102) {
    // Save supported hal pixel formats for bit depth of 10, the first time this is called
    if (!mBitDepth10HalPixelFormats.size()) {
        std::vector<int> halPixelFormats;
        halPixelFormats.push_back(HAL_PIXEL_FORMAT_YCBCR_P010);

        // since allowRGBA1010102 can chance in each call, but mBitDepth10HalPixelFormats
        // is populated only once, allowRGBA1010102 is not considered at this stage.
        halPixelFormats.push_back(HAL_PIXEL_FORMAT_RGBA_1010102);

        for (int halPixelFormat : halPixelFormats) {
            if (isHalPixelFormatSupported((AHardwareBuffer_Format)halPixelFormat)) {
                mBitDepth10HalPixelFormats.push_back(halPixelFormat);
            }
        }
        // Add YV12 in the end as a fall-back option
        mBitDepth10HalPixelFormats.push_back(HAL_PIXEL_FORMAT_YV12);
    }
    // From Android T onwards, HAL_PIXEL_FORMAT_RGBA_1010102 corresponds to true
    // RGBA 1010102 format unlike earlier versions where it was used to represent
    // YUVA 1010102 data
    if (!isAtLeastT()) {
        // When RGBA1010102 is not allowed and if the first supported hal pixel is format is
        // HAL_PIXEL_FORMAT_RGBA_1010102, then return HAL_PIXEL_FORMAT_YV12
        if (!allowRGBA1010102 && mBitDepth10HalPixelFormats[0] == HAL_PIXEL_FORMAT_RGBA_1010102) {
            return HAL_PIXEL_FORMAT_YV12;
        }
    }
    // Return the first entry from supported formats
    return mBitDepth10HalPixelFormats[0];
}
std::shared_ptr<C2Buffer> C2AudioDecComponent::createLinearBuffer(
        const std::shared_ptr<C2LinearBlock> &block, size_t offset, size_t size) {
    return C2Buffer::CreateLinearBuffer(block->share(offset, size, ::C2Fence()));
}

std::shared_ptr<C2Buffer> C2AudioDecComponent::createGraphicBuffer(
        const std::shared_ptr<C2GraphicBlock> &block, const C2Rect &crop) {
    return C2Buffer::CreateGraphicBuffer(block->share(crop, ::C2Fence()));
}

} // namespace android
