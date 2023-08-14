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

#define LOG_NDEBUG 0

#define LOG_TAG "TunnelHelper"

#include <base/bind.h>
#include <base/bind_helpers.h>
#include <C2AllocatorGralloc.h>

#include <am_gralloc_ext.h>

#include <C2VendorProperty.h>
#include <c2logdebug.h>
#include <C2VdecTunnelHelper.h>
#include <C2VdecInterfaceImpl.h>
#include <C2VdecDebugUtil.h>
#include <C2VdecDeviceUtil.h>
#include <inttypes.h>

#define C2VdecTMH_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, comp->mSessionID, comp->mDecoderID, ##str)

namespace android {

#define RETURN_ON_UNINITIALIZED_OR_ERROR()                                 \
    do {                                                                   \
        if (comp->mHasError \
            || comp->mComponentState == ComponentState::UNINITIALIZED \
            || comp->mComponentState == ComponentState::DESTROYING \
            || comp->mComponentState == ComponentState::DESTROYED) \
            return;                                                        \
    } while (0)

#define LockWeakPtrWithReturnVal(name, weak, retval) \
    auto name = weak.lock(); \
    if (name == nullptr) { \
        C2VdecTMH_LOG(CODEC2_LOG_ERR, "[%s:%d] null ptr, please check", __func__, __LINE__); \
        return retval;\
    }

#define LockWeakPtrWithReturnVoid(name, weak) \
    auto name = weak.lock(); \
    if (name == nullptr) { \
        C2VdecTMH_LOG(CODEC2_LOG_ERR, "[%s:%d] null ptr, please check", __func__, __LINE__); \
        return;\
    }

C2VdecComponent::TunnelHelper::TunnelHelper(bool secure) {
    mSecure = secure;
    mReallocWhenResChange = false;
    mReallocWhenResChange = property_get_bool(C2_PROPERTY_VDEC_REALLOC_TUNNEL_RESCHANGE, mReallocWhenResChange);
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    mPixelFormat = 0;
    mOutBufferCount = 0;

    mSyncId = 0;
    mTunnelId = 0;
    mTunnelHandle = NULL;
    CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
}

C2VdecComponent::TunnelHelper::~TunnelHelper() {
    stop();

    CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
}

c2_status_t C2VdecComponent::TunnelHelper::setComponent(std::shared_ptr<C2VdecComponent> sharedcomp) {
    mComp = sharedcomp;
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);

    mSyncId = comp->mSyncId;
    mIntfImpl = comp->GetIntfImpl();
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, C2_BAD_VALUE);

    if (intfImpl != NULL && intfImpl->getInputCodec() == InputCodec::H264) {
        mReallocWhenResChange = true;
    }

    mVideoTunnelRenderer = std::make_shared<VideoTunnelRendererWraper>(mSecure);
    if (mVideoTunnelRenderer) {
        mTunnelId = mVideoTunnelRenderer->getTunnelId();
        mTunnelHandle = am_gralloc_create_sideband_handle(AM_FIXED_TUNNEL, mTunnelId);
        if (mTunnelHandle != NULL && intfImpl!= NULL && intfImpl->mTunnelHandleOutput != NULL) {
            //CHECK_EQ(intfImpl->mTunnelHandleOutput->flexCount(), mTunnelHandle->numInts);
            memcpy(intfImpl->mTunnelHandleOutput->m.values, &mTunnelHandle->data[mTunnelHandle->numFds], sizeof(int32_t) * mTunnelHandle->numInts);
        }
    }

    C2VdecTMH_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);

    return C2_OK;
}


c2_status_t C2VdecComponent::TunnelHelper::start() {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    if (mVideoTunnelRenderer) {
        if (mVideoTunnelRenderer->init(mSyncId) == false) {
           C2VdecTMH_LOG(CODEC2_LOG_ERR, "Tunnel render init failed");
        }

        mDeviceUtil = comp->mDeviceUtil;
        mVideoTunnelRenderer->regFillVideoFrameCallBack(fillVideoFrameCallback2, this);
        mVideoTunnelRenderer->regNotifyTunnelRenderTimeCallBack(notifyTunnelRenderTimeCallback, this);
        mVideoTunnelRenderer->regNotifyEventCallBack(notifyTunnelEventCallback, this);
        mVideoTunnelRenderer->start();
    }

    return C2_OK;
}

c2_status_t C2VdecComponent::TunnelHelper::stop() {
    mTunnelAbandonMediaTimeQueue.clear();

    for (auto iter = mOutBufferFdMap.begin(); iter != mOutBufferFdMap.end(); iter++) {
        if (iter->first >= 0) {
            close(iter->first);
        }
    }
    mOutBufferFdMap.clear();

    if (mVideoTunnelRenderer) {
        mVideoTunnelRenderer->regFillVideoFrameCallBack(NULL, NULL);
        mVideoTunnelRenderer->regNotifyTunnelRenderTimeCallBack(NULL, NULL);
        mVideoTunnelRenderer->regNotifyEventCallBack(NULL, NULL);

        mVideoTunnelRenderer->stop();
        mVideoTunnelRenderer.reset();
        mVideoTunnelRenderer = NULL;
        if (mTunnelHandle) {
            am_gralloc_destroy_sideband_handle(mTunnelHandle);
            mTunnelHandle = NULL;
        }
    }
    return C2_OK;
}


bool C2VdecComponent::TunnelHelper::isInResolutionChanging() {
    LockWeakPtrWithReturnVal(comp, mComp, false);
    return (comp->mGraphicBlocks.size() < mOutBufferCount);
}

void C2VdecComponent::TunnelHelper::onFillVideoFrameTunnel2(int dmafd, bool rendered) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    scoped_refptr<::base::SingleThreadTaskRunner> taskRunner = comp->GetTaskRunner();
    if (taskRunner == nullptr) {
        return;
    }
    DCHECK(taskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();
    C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Fd:%d, render:%d", __func__, __LINE__, dmafd, rendered);

    struct fillVideoFrame2 frame = {
        .fd = dmafd,
        .rendered = rendered
    };

    mFillVideoFrameQueue.push_back(frame);

    if (!comp->mCanQueueOutBuffer) {
        C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Cannot queue out buffer, cache it fd:%d, render:%d",
            dmafd, rendered);
        return;
    }

    for (auto &frame : mFillVideoFrameQueue) {
        if (isInResolutionChanging()) {
            C2BlockPool::local_id_t poolId = 0;

            //check fd is new size or old
            LockWeakPtrWithReturnVoid(blockPoolUtil, mBlockPoolUtil);
            blockPoolUtil->getPoolId(&poolId);
            auto iter = mOutBufferFdMap.find(dmafd);
            DCHECK(iter != mOutBufferFdMap.end());

            GraphicBlockInfo *info = comp->getGraphicBlockByFd(frame.fd);
            GraphicBlockInfo *info2 = comp->getGraphicBlockByBlockId(poolId, iter->second.mBlockId);
            int fd = -1;
            if ((info == NULL) ||
                (info2 == NULL) ||
                (info != info2)) {
                media::Size& size = comp->mOutputFormat.mCodedSize;

                if (iter->first >= 0) {
                    close(iter->first);
                }
                GraphicBlockStateDec(comp, GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER);
                C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s:%d] Close fd:%d, block id:%d", __func__, __LINE__, iter->first, iter->second.mBlockId);
                mOutBufferFdMap.erase(iter);

                allocTunnelBuffer(size, mPixelFormat, &fd);
                GraphicBlockInfo *info = comp->getGraphicBlockByFd(fd);
                if (info != NULL) {
                    GraphicBlockStateInit(comp, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
                    BufferStatus(comp, CODEC2_LOG_TAG_BUFFER, "tunnel new allocate fd=%d, index=%d", info->mFd, info->mBlockId);
                    comp->sendOutputBufferToAccelerator(info, true /*ownByAccelerator*/);
                }
                continue;
            }
        }

        GraphicBlockInfo* info = comp->getGraphicBlockByFd(frame.fd);
        if (info == NULL) {
            C2VdecTMH_LOG(CODEC2_LOG_ERR, "[%s:%d] Cannot get graphicblock according fd:%d", __func__, __LINE__, dmafd);
            comp->reportError(C2_CORRUPTED);
            return;
        }

        GraphicBlockStateChange(comp, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
        BufferStatus(comp, CODEC2_LOG_TAG_BUFFER, "tunnel reuse buffer fd=%d, index=%d", info->mFd, info->mBlockId);
        comp->sendOutputBufferToAccelerator(info, true /* ownByAccelerator */);

        /* for drop, need report finished work */
        if (!frame.rendered) {
            auto pendingBuffer = std::find_if(
                    comp->mPendingBuffersToWork.begin(), comp->mPendingBuffersToWork.end(),
                    [id = info->mBlockId](const OutputBufferInfo& o) { return o.mBlockId == id;});
            if (pendingBuffer != comp->mPendingBuffersToWork.end()) {
                struct renderTime rendertime = {
                    .mediaUs = (int64_t)pendingBuffer->mMediaTimeUs,
                    .renderUs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000,
                };
                sendOutputBufferToWorkTunnel(&rendertime);
            }
        }
    }
    mFillVideoFrameQueue.clear();
}

int C2VdecComponent::TunnelHelper::postFillVideoFrameTunnel2(int dmafd, bool rendered) {
    LockWeakPtrWithReturnVal(comp, mComp, 0);
    scoped_refptr<::base::SingleThreadTaskRunner> taskRunner = comp->GetTaskRunner();
    if (taskRunner == nullptr) {
        return 0;
    }
    taskRunner->PostTask(FROM_HERE,
        ::base::Bind(&C2VdecComponent::TunnelHelper::onFillVideoFrameTunnel2, ::base::Unretained(this),
            dmafd, rendered));
    return 0;
}

int C2VdecComponent::TunnelHelper::fillVideoFrameCallback2(void* obj, void* args) {
    C2VdecComponent::TunnelHelper* pTunnelHelper = (C2VdecComponent::TunnelHelper*)obj;
    struct fillVideoFrame2* pfillVideoFrame = (struct fillVideoFrame2*)args;

    pTunnelHelper->postFillVideoFrameTunnel2(pfillVideoFrame->fd, pfillVideoFrame->rendered);

    return 0;
}

void C2VdecComponent::TunnelHelper::onNotifyRenderTimeTunnel(struct renderTime rendertime) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    scoped_refptr<::base::SingleThreadTaskRunner> taskRunner = comp->GetTaskRunner();
    if (taskRunner == nullptr) {
        return;
    }
    DCHECK(taskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    struct renderTime renderTime = {
        .mediaUs = rendertime.mediaUs,
        .renderUs = rendertime.renderUs,
    };
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s:%d] Rendertime:%" PRId64 "", __func__, __LINE__, renderTime.mediaUs);
    sendOutputBufferToWorkTunnel(&renderTime);
}

int C2VdecComponent::TunnelHelper::postNotifyRenderTimeTunnel(struct renderTime* rendertime) {
    LockWeakPtrWithReturnVal(comp, mComp, 0);
    scoped_refptr<::base::SingleThreadTaskRunner> taskRunner = comp->GetTaskRunner();
    if (taskRunner == nullptr) {
        return -1;
    }
    struct renderTime renderTime = {
        .mediaUs = rendertime->mediaUs,
        .renderUs = rendertime->renderUs,
    };
    taskRunner->PostTask(FROM_HERE,
        ::base::Bind(&C2VdecComponent::TunnelHelper::onNotifyRenderTimeTunnel, ::base::Unretained(this),
            ::base::Passed(&renderTime)));
    return 0;
}

int C2VdecComponent::TunnelHelper::notifyTunnelRenderTimeCallback(void* obj, void* args) {
    C2VdecComponent::TunnelHelper* pTunnelHelper = (C2VdecComponent::TunnelHelper*)obj;
    struct renderTime* rendertime = (struct renderTime*)args;
    pTunnelHelper->postNotifyRenderTimeTunnel(rendertime);
    return 0;
}

void C2VdecComponent::TunnelHelper::onNotifyTunnelEvent(struct tunnelEventParam param) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    LockWeakPtrWithReturnVoid(deviceUtil, mDeviceUtil);
    switch (param.type) {
        case android::VideoTunnelRendererWraper::CB_EVENT_UNDERFLOW: {
            uint32_t* data = (uint32_t*)param.data;
            comp->mTunnelUnderflow = data[0];
            free(data);
            data = NULL;
            CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "tunnel underflow %d!", comp->mTunnelUnderflow);
            deviceUtil->checkConfigInfoFromDecoderAndReconfig(TUNNEL_UNDERFLOW);
            break;
        }
        default:
            break;
    }
}

int C2VdecComponent::TunnelHelper::postNotifyTunnelEvent(struct tunnelEventParam* param) {
    LockWeakPtrWithReturnVal(comp, mComp, -1);
    if (param == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "%s param error, please check it.", __func__);
        return -1;
    }
    scoped_refptr<::base::SingleThreadTaskRunner> taskRunner = comp->GetTaskRunner();
    if (taskRunner == nullptr) {
        return -1;
    }

    struct tunnelEventParam eventParam = {
        .type = param->type,
        .paramSize = param->paramSize,
    };
    eventParam.data = malloc(eventParam.paramSize);
    if (eventParam.data == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "%s malloc failed, please check it.", __func__);
        return -1;
    }
    memcpy(eventParam.data, param->data, eventParam.paramSize);
    taskRunner->PostTask(FROM_HERE,
        ::base::Bind(&C2VdecComponent::TunnelHelper::onNotifyTunnelEvent, ::base::Unretained(this),
        ::base::Passed(&eventParam)));

    return 0;
}

int C2VdecComponent::TunnelHelper::notifyTunnelEventCallback(void* obj, void* args) {
    if (obj == NULL || args == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "%s obj or args error, please check it.", __func__);
        return -1;
    }
    C2VdecComponent::TunnelHelper* pTunnelHelper = (C2VdecComponent::TunnelHelper*)obj;
    struct tunnelEventParam* eventParam = (struct tunnelEventParam*)args;
    pTunnelHelper->postNotifyTunnelEvent(eventParam);
    return 0;
}

c2_status_t C2VdecComponent::TunnelHelper::sendVideoFrameToVideoTunnel(int32_t pictureBufferId, int64_t bitstreamId, uint64_t timestamp) {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, C2_BAD_VALUE);
    DCHECK(mVideoTunnelRenderer != NULL);

    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] PictureId:%d, bitstreamId:%" PRId64 "", __func__, __LINE__,
            pictureBufferId, bitstreamId);
    GraphicBlockInfo* info = comp->getGraphicBlockById(pictureBufferId);
    if (info == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "[%s:%d] PictureId:%d not found", __func__, __LINE__, pictureBufferId);
        return C2_BAD_STATE;
    }
    if (info->mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR ||
        info->mState == GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER) {
        C2VdecTMH_LOG(CODEC2_LOG_ERR, "Graphic block (id=%d) should not be owned by accelerator or tunnelrender", info->mBlockId);
        return C2_BAD_STATE;
    }


    // implement Android Video Peek
    C2Work* work = comp->getPendingWorkByBitstreamId(bitstreamId);
    if (work) {
        bool frameHoldRender = false;
        for (const std::unique_ptr<C2Param> &param : work->input.configUpdate) {
            switch (param->coreIndex().coreIndex()) {
                case C2StreamTunnelHoldRender::CORE_INDEX:
                    {
                        C2StreamTunnelHoldRender::input firstTunnelFrameHoldRender;
                        if (!firstTunnelFrameHoldRender.updateFrom(*param)) break;
                        if (firstTunnelFrameHoldRender.value == C2_TRUE) {
                            frameHoldRender = true;
                            C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] work has frameHoldRender", __func__, __LINE__);
                            break;
                        }
                    }
                    break;
                default:
                    break;
            }
        }
        if (frameHoldRender == true) {
            work = comp->cloneWork(work);
            if (work != NULL) {
                std::unique_ptr<C2StreamTunnelHoldRender::output> frameReady = std::make_unique<C2StreamTunnelHoldRender::output>();
                frameReady->value = C2_TRUE;
                work->worklets.front()->output.configUpdate.push_back(std::move(frameReady));
                comp->sendClonedWork(work, 0);
                C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Send cloned work for FirstFrameReady", __func__, __LINE__);
            }
        }
    }

    if (mVideoTunnelRenderer) {
        GraphicBlockStateChange(comp, info, GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER);
        BufferStatus(comp, CODEC2_LOG_TAG_BUFFER, "tunnel send to videotunnel fd=%d, pts=%" PRId64"", info->mFd, timestamp);
        if (intfImpl->mVendorNetflixVPeek->vpeek == true) {
            //netflix vpeek need render at once.
            mVideoTunnelRenderer->sendVideoFrame(info->mFd, timestamp, true);
            intfImpl->mVendorNetflixVPeek->vpeek = false;
        } else {
            mVideoTunnelRenderer->sendVideoFrame(info->mFd, timestamp);
        }
    }

    return C2_OK;
}

c2_status_t C2VdecComponent::TunnelHelper::flush() {
    if (mVideoTunnelRenderer) {
        mVideoTunnelRenderer->flush();
    }

    return C2_OK;
}

c2_status_t C2VdecComponent::TunnelHelper::sendOutputBufferToWorkTunnel(struct renderTime* rendertime) {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal(intfImpl, mIntfImpl, C2_BAD_VALUE);
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Rendertime:%" PRId64 "", __func__, __LINE__, rendertime->mediaUs);

    if (comp->mPendingBuffersToWork.empty() ||
        comp->mPendingWorks.empty()) {
        C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Empty pendingwork, ignore report it");
        return C2_OK;
    }

    auto pendingBuffer = comp->findPendingBuffersToWorkByTime(rendertime->mediaUs);
    int32_t renderBitstreamId = pendingBuffer->mBitstreamId;
    C2Work* work = comp->getPendingWorkByBitstreamId(renderBitstreamId);
    if (work == NULL) {
        for (auto it = mTunnelAbandonMediaTimeQueue.begin(); it != mTunnelAbandonMediaTimeQueue.end(); it++) {
            if (rendertime->mediaUs == *it) {
                mTunnelAbandonMediaTimeQueue.erase(it);
                CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Not find the correct work with mediaTime:%" PRId64", correct work have abandoned and report to framework", rendertime->mediaUs);
                comp->erasePendingBuffersToWorkByTime(rendertime->mediaUs);
                return C2_OK;
            }
        }
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "not found corresponded work with mediaTime:%lld", (long long)rendertime->mediaUs);
        comp->erasePendingBuffersToWorkByTime(rendertime->mediaUs);
        return C2_OK;
    }

    intfImpl->mTunnelSystemTimeOut->value = rendertime->renderUs * 1000;
    work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*(intfImpl->mTunnelSystemTimeOut)));

    if (pendingBuffer != comp->mPendingBuffersToWork.end()) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Rendertime:%" PRId64 ", bitstreamId:%d, flags:%d", __func__, __LINE__, rendertime->mediaUs, pendingBuffer->mBitstreamId,pendingBuffer->flags);
        comp->reportWorkIfFinished(pendingBuffer->mBitstreamId,pendingBuffer->flags);
        comp->mPendingBuffersToWork.erase(pendingBuffer);
        /* EOS work check */
        if ((comp->mPendingWorks.size() == 1u) &&
            comp->mPendingOutputEOS) {
            C2Work* eosWork = comp->mPendingWorks.front().get();
            DCHECK((eosWork->input.flags & C2FrameData::FLAG_END_OF_STREAM) > 0);
            intfImpl->mTunnelSystemTimeOut->value = systemTime(SYSTEM_TIME_MONOTONIC);
            eosWork->worklets.front()->output.ordinal.timestamp = INT64_MAX;
            eosWork->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*(intfImpl->mTunnelSystemTimeOut)));
            C2VdecTMH_LOG(CODEC2_LOG_INFO, "[%s:%d] eos work report", __func__, __LINE__);
            comp->reportEOSWork();
        }
    }

    return C2_OK;
}


c2_status_t C2VdecComponent::TunnelHelper::storeAbandonedFrame(int64_t timeus) {
    mTunnelAbandonMediaTimeQueue.push_back(timeus);
    return C2_OK;
}

c2_status_t C2VdecComponent::TunnelHelper::allocTunnelBuffersAndSendToDecoder(const media::Size& size, uint32_t pixelFormat) {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    mBlockPoolUtil = comp->mBlockPoolUtil;
    LockWeakPtrWithReturnVal(blockPoolUtil, mBlockPoolUtil, C2_BAD_VALUE);

    mPixelFormat = pixelFormat;
    mOutBufferCount = comp->mOutBufferCount;

    blockPoolUtil->requestNewBufferSet(mOutBufferCount);
    if (comp->mVideoDecWraper) {
        comp->mVideoDecWraper->assignPictureBuffers(mOutBufferCount);
        comp->mCanQueueOutBuffer = true;
    }

    allocTunnelBufferAndSendToDecoder(size, pixelFormat, 0);
    return C2_OK;
}

c2_status_t C2VdecComponent::TunnelHelper::allocTunnelBuffer(const media::Size& size, uint32_t pixelFormat, int* pFd) {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    if (pFd == NULL) {
        C2VdecTMH_LOG(CODEC2_LOG_ERR, "[%s] alloc buffer fd error, please check!", __func__);
        return C2_BAD_VALUE;
    }

    LockWeakPtrWithReturnVal(blockPoolUtil, mBlockPoolUtil, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal(deviceUtil, mDeviceUtil, C2_BAD_VALUE);

    uint64_t platformUsage = deviceUtil->getPlatformUsage();
    C2MemoryUsage usage = {
            mSecure ? (C2MemoryUsage::READ_PROTECTED | C2MemoryUsage::WRITE_PROTECTED) :
            (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE),  platformUsage};

    C2BlockPool::local_id_t poolId = -1;
    uint32_t blockId = -1;
    int fd = -1;
    std::shared_ptr<C2GraphicBlock> c2Block;

    blockPoolUtil->getPoolId(&poolId);
    auto err = blockPoolUtil->fetchGraphicBlock(size.width(), size.height(), pixelFormat, usage, &c2Block);
    if (err != C2_OK) {
        C2VdecTMH_LOG(CODEC2_LOG_ERR, "[%s] alloc buffer failed, please check!", __func__);
    } else {
        err = blockPoolUtil->getBlockIdByGraphicBlock(c2Block, &blockId);
        if (err != C2_OK) {
            C2VdecTMH_LOG(CODEC2_LOG_ERR, "[%s] get the block id failed, please check!", __func__);
            return C2_BAD_VALUE;
        }
        blockPoolUtil->getBlockFd(c2Block, &fd);

        int dupfd = dup(fd);
        DCHECK(dupfd >= 0);
        appendTunnelOutputBuffer(c2Block, dupfd, blockId, poolId);
        mOutBufferFdMap.insert(std::pair<int, TunnelFdInfo>(dupfd, TunnelFdInfo(fd, blockId)));
        *pFd = dupfd;
        C2VdecTMH_LOG(CODEC2_LOG_INFO, "[%s:%d] alloc buffer fd:%d(%d), blockId:%d", __func__, __LINE__, dupfd, fd, blockId);
    }

    return C2_OK;
}

void C2VdecComponent::TunnelHelper::allocTunnelBufferAndSendToDecoder(const media::Size& size, uint32_t pixelFormat, int index) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    LockWeakPtrWithReturnVoid(intfImpl, mIntfImpl);
    scoped_refptr<::base::SingleThreadTaskRunner> taskRunner = comp->GetTaskRunner();
    if (taskRunner == nullptr) {
        return;
    }

    C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL2, "%s#%d create buffer#%d", __func__, __LINE__, index);
    if (index >= mOutBufferCount) {
        return;
    }

    int fd = -1;
    allocTunnelBuffer(size, pixelFormat, &fd);
    GraphicBlockInfo* info = comp->getGraphicBlockByFd(fd);
    if (info == NULL) {
        return;
    }
    GraphicBlockStateInit(comp, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
    BufferStatus(comp, CODEC2_LOG_TAG_BUFFER, "tunnel allocate fd=%d, index=%d", info->mFd, info->mBlockId);

    comp->sendOutputBufferToAccelerator(info, true);
    taskRunner->PostTask(FROM_HERE,
        ::base::Bind(&C2VdecComponent::TunnelHelper::allocTunnelBufferAndSendToDecoder, ::base::Unretained(this),
        size, pixelFormat, index+1));

    return;
}

c2_status_t C2VdecComponent::TunnelHelper::videoResolutionChangeTunnel() {
    bool sizeChanged = false;
    bool bufferNumEnlarged = false;
    bool bufferNumSet = false;
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    LockWeakPtrWithReturnVal(blockPoolUtil, mBlockPoolUtil, C2_BAD_VALUE)
    LockWeakPtrWithReturnVal(deviceUtil, mDeviceUtil, C2_BAD_VALUE);

    uint64_t platformUsage = deviceUtil->getPlatformUsage();
    C2MemoryUsage usage = {
            mSecure ? (C2MemoryUsage::READ_PROTECTED | C2MemoryUsage::WRITE_PROTECTED) :
            (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE),  platformUsage};
    media::Size& size = comp->mOutputFormat.mCodedSize;
    comp->mPendingOutputFormat.reset();


    C2VdecTMH_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
    for (auto& info : comp->mGraphicBlocks) {
        info.mFdHaveSet = false;
    }

    C2VdecTMH_LOG(CODEC2_LOG_INFO, "[%s:%d] in resolution changing:%d", __func__, __LINE__, isInResolutionChanging());
    if (checkReallocOutputBuffer(comp->mLastOutputFormat, comp->mOutputFormat, &sizeChanged, &bufferNumEnlarged)) {
        if (mReallocWhenResChange && sizeChanged) {
            //all realloc buffer
            int alloc_first = 0;
            int fd = -1;
            for (auto& info : comp->mGraphicBlocks) {
                if (info.mState != GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER) {
                    GraphicBlockInfo* info1 = &info;
                    GraphicBlockStateChange(comp, info1, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
                    auto iter = mOutBufferFdMap.find(info.mFd);
                    if (iter != mOutBufferFdMap.end()) {
                        C2VdecTMH_LOG(CODEC2_LOG_INFO, " [%s:%d] close fd:%d, blockid:%d", __func__, __LINE__, iter->first, iter->second.mBlockId);
                        close(iter->first);
                        mOutBufferFdMap.erase(iter);
                        alloc_first ++;
                        GraphicBlockStateReset(comp, info1);
                    } else {
                        C2VdecTMH_LOG(CODEC2_LOG_ERR, " [%s:%d] don't found buffer, please check it.", __func__, __LINE__);
                    }
                }
                info.mGraphicBlock.reset();
            }
            comp->mGraphicBlocks.clear();
            resetBlockPoolBuffers();
            if (bufferNumEnlarged) {
                alloc_first += comp->mOutputFormat.mMinNumBuffers - comp->mLastOutputFormat.mMinNumBuffers;
                mOutBufferCount = comp->mOutputFormat.mMinNumBuffers;
            }
            blockPoolUtil->requestNewBufferSet(mOutBufferCount);
            bufferNumSet = true;
            for (int i = 0; i < alloc_first; i++) {
                allocTunnelBuffer(size, mPixelFormat, &fd);
                GraphicBlockInfo* info = comp->getGraphicBlockByFd(fd);
                if (info != NULL) {
                    GraphicBlockStateInit(comp, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
                    BufferStatus(comp, CODEC2_LOG_TAG_BUFFER, "tunnel new allocate fd=%d, index=%d", info->mFd, info->mBlockId);
                }
            }
        } else if (bufferNumEnlarged) {
            //add new allocate buffer
            for (auto& info : comp->mGraphicBlocks) {
                info.mFdHaveSet = false;
            }
            int32_t lastBufferCount = comp->mLastOutputFormat.mMinNumBuffers;
            mOutBufferCount = comp->mOutputFormat.mMinNumBuffers;

            int fd = -1;
            for (int i = lastBufferCount; i < mOutBufferCount; i++) {
                allocTunnelBuffer(size, mPixelFormat, &fd);
                GraphicBlockInfo* info = comp->getGraphicBlockByFd(fd);
                if (info != NULL) {
                    GraphicBlockStateInit(comp, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
                    BufferStatus(comp, CODEC2_LOG_TAG_BUFFER, "tunnel new allocate fd=%d, index=%d", info->mFd, info->mBlockId);
                }
            }
        }
    }

    if (!bufferNumSet) {
        blockPoolUtil->requestNewBufferSet(mOutBufferCount);
    }
    if (comp->mVideoDecWraper) {
        comp->mVideoDecWraper->assignPictureBuffers(mOutBufferCount);
        comp->mCanQueueOutBuffer = true;
    }

    for (auto& info : comp->mGraphicBlocks) {
        if (info.mState == GraphicBlockInfo::State::OWNED_BY_COMPONENT) {
            comp->sendOutputBufferToAccelerator(&info, true);
        }
    }

    return C2_OK;
}

void C2VdecComponent::TunnelHelper::onAndroidVideoPeek() {
    if (mVideoTunnelRenderer) {
        mVideoTunnelRenderer->peekFirstFrame();
    }
}

c2_status_t C2VdecComponent::TunnelHelper::resetBlockPoolBuffers() {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    comp->resetBlockPoolUtil();
    mBlockPoolUtil = comp->mBlockPoolUtil;
    return C2_OK;
}


bool C2VdecComponent::TunnelHelper::checkReallocOutputBuffer(VideoFormat video_format_old,
                VideoFormat video_format_new,
                bool *sizeChanged, bool *bufferNumEnlarged) {
    bool bufferNumIncrease = false;
    bool frameSizeChanged = false;
    LockWeakPtrWithReturnVal(comp, mComp, false);

    C2VdecTMH_LOG(CODEC2_LOG_INFO, "[%s] %dx%d(%d)->%dx%d(%d)", __func__,
                 video_format_old.mCodedSize.width(),
                 video_format_old.mCodedSize.height(),
                 video_format_old.mMinNumBuffers,
                 video_format_new.mCodedSize.width(),
                 video_format_new.mCodedSize.height(),
                 video_format_new.mMinNumBuffers);
    if (video_format_new.mMinNumBuffers > video_format_old.mMinNumBuffers) {
        bufferNumIncrease = true;
        C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Buffer num enlarged");
    }
    if (video_format_new.mCodedSize.width() != video_format_old.mCodedSize.width() ||
        video_format_new.mCodedSize.height() !=  video_format_old.mCodedSize.height()) {
        frameSizeChanged = true;
        C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Frame size changed");
    }

    *sizeChanged = frameSizeChanged;
    *bufferNumEnlarged = bufferNumIncrease;

    if (bufferNumIncrease || frameSizeChanged) {
        return true;
    }

    return false;
}

void C2VdecComponent::TunnelHelper::appendTunnelOutputBuffer(std::shared_ptr<C2GraphicBlock> block, int fd, uint32_t blockId, uint32_t poolId) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    C2VdecComponent::GraphicBlockInfo info;
    info.mGraphicBlock = std::move(block);
    info.mBlockId = blockId;
    info.mPoolId = poolId;
    info.mFd = fd;
    info.mNeedRealloc = false;
    info.mFdHaveSet = false;
    comp->mGraphicBlocks.push_back(std::move(info));
}

c2_status_t C2VdecComponent::TunnelHelper::fastHandleWorkTunnel(int64_t bitstreamId, int32_t pictureBufferId) {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);

    auto workIter = comp->findPendingWorkByBitstreamId(bitstreamId);
    if (workIter == comp->mPendingWorks.end()) {
        CODEC2_LOG(CODEC2_LOG_ERR, "[%s:%d] Can not find work with bistreamId:%" PRId64 ", please check!", __func__, __LINE__, bitstreamId);
        return C2_BAD_VALUE;
    }

    C2Work* work = workIter->get();
    if (work == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "[%s:%d] Can not find work, please check!", __func__, __LINE__);
        return C2_BAD_VALUE;
    }

    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s:%d] bistreamId:%" PRId64 ", pictureId:%d", __func__, __LINE__,
            bitstreamId, pictureBufferId);
    DCHECK((work->input.flags & C2FrameData::FLAG_DROP_FRAME)
            || (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG));

    work->result = C2_OK;
    work->workletsProcessed = static_cast<uint32_t>(work->worklets.size());
    work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(0);
    comp->reportWork(std::move(*workIter));
    comp->mOutputFinishedWorkCount++;
    comp->mPendingWorks.erase(workIter);

    return C2_OK;
}

c2_status_t C2VdecComponent::TunnelHelper::fastHandleOutBufferTunnel(uint64_t timestamp, int32_t pictureBufferId) {
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);

    if (comp->mPendingBuffersToWork.empty()) {
        C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Empty BuffersToWork, ignore it");
        return C2_OK;
    }

    C2Work* work = NULL;
    auto pendingBuffer = comp->findPendingBuffersToWorkByTime(timestamp);
    auto workIter = comp->findPendingWorkByBitstreamId(pendingBuffer->mBitstreamId);
    if (workIter != comp->mPendingWorks.end()) {
        work = workIter->get();
    }

    if (work != NULL) {
        work->result = C2_OK;
        work->workletsProcessed = static_cast<uint32_t>(work->worklets.size());
        work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(0);
        comp->reportWork(std::move(*workIter));
        comp->mOutputFinishedWorkCount++;
        comp->mPendingWorks.erase(workIter);
    }

    GraphicBlockInfo* info = comp->getGraphicBlockById(pictureBufferId);
    if (!info) {
        C2VdecTMH_LOG(CODEC2_LOG_ERR, "Can't get graphic block pictureBufferId:%d, please check!", pictureBufferId);
        return C2_BAD_VALUE;
    }

    if (info->mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR) {
        GraphicBlockStateChange(comp, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
    }

    comp->sendOutputBufferToAccelerator(info, true);
    comp->erasePendingBuffersToWorkByTime(timestamp);
    BufferStatus(comp, CODEC2_LOG_TAG_BUFFER, "tunnel drop and reuse fd=%d, index=%d", info->mFd, info->mBlockId);
    return C2_OK;
}

void C2VdecComponent::TunnelHelper::configureEsModeHwAvsyncId(int32_t avSyncId){
    mSyncId = avSyncId;
}

void C2VdecComponent::TunnelHelper::videoSyncQueueVideoFrame(int64_t timestampUs, uint32_t size) {
    if (mVideoTunnelRenderer) {
        mVideoTunnelRenderer->videoSyncQueueVideoFrame((timestampUs*9/100),size);
    }

}


}
