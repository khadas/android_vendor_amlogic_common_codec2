/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
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
#include <inttypes.h>

#define C2VdecTMH_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, mComp->mCurInstanceID, C2VdecComponent::mInstanceNum, ##str)

namespace android {

#define RETURN_ON_UNINITIALIZED_OR_ERROR()                                 \
    do {                                                                   \
        if (mComp->mHasError \
            || mComp->mComponentState == ComponentState::UNINITIALIZED \
            || mComp->mComponentState == ComponentState::DESTROYING \
            || mComp->mComponentState == ComponentState::DESTROYED) \
            return;                                                        \
    } while (0)


C2VdecComponent::TunnelHelper::TunnelHelper(C2VdecComponent* comp, bool secure):
    mComp(comp),
    mSecure(secure) {
    DCHECK(mComp != NULL);
    mSyncId = mComp->mSyncId;
    mTaskRunner = mComp->GetTaskRunner();
    DCHECK(mTaskRunner != NULL);

    mVideoTunnelRenderer = new VideoTunnelRendererWraper(mSecure);
    mIntfImpl = mComp->GetIntfImpl();
    DCHECK(mIntfImpl != NULL);
    mReallocWhenResChange = false;

    if (mIntfImpl->getInputCodec() == InputCodec::H264) {
        mReallocWhenResChange = true;
    }
    mReallocWhenResChange = property_get_bool(C2_PROPERTY_VDEC_REALLOC_TUNNEL_RESCHANGE, mReallocWhenResChange);

    if (mVideoTunnelRenderer) {
        mTunnelId = mVideoTunnelRenderer->getTunnelId();
        mTunnelHandle = am_gralloc_create_sideband_handle(AM_FIXED_TUNNEL, mTunnelId);
        if (mTunnelHandle) {
            CHECK_EQ(mIntfImpl->mTunnelHandleOutput->flexCount(), mTunnelHandle->numInts);
            memcpy(mIntfImpl->mTunnelHandleOutput->m.values, &mTunnelHandle->data[mTunnelHandle->numFds], sizeof(int32_t) * mTunnelHandle->numInts);
        }
    }
    mAndroidPeekFrameReady = false;
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    C2VdecTMH_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
}

C2VdecComponent::TunnelHelper::~TunnelHelper() {
    stop();

    mTaskRunner = NULL;
    mIntfImpl = NULL;
    C2VdecTMH_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
}

bool C2VdecComponent::TunnelHelper::start() {
    if (mVideoTunnelRenderer) {
        if (mVideoTunnelRenderer->init(mSyncId) == false) {
           C2VdecTMH_LOG(CODEC2_LOG_ERR, "Tunnel render init failed");
        }
        mVideoTunnelRenderer->regFillVideoFrameCallBack(fillVideoFrameCallback2, this);
        mVideoTunnelRenderer->regNotifyTunnelRenderTimeCallBack(notifyTunnelRenderTimeCallback, this);
        mVideoTunnelRenderer->start();
    }

    return true;
}

bool C2VdecComponent::TunnelHelper::stop() {
    mTunnelAbandonMediaTimeQueue.clear();

    for (auto iter = mOutBufferFdMap.begin(); iter != mOutBufferFdMap.end(); iter++) {
        if (iter->first >= 0) {
            close(iter->first);
        }
    }
    mOutBufferFdMap.clear();
    mAndroidPeekFrameReady = false;

    if (mVideoTunnelRenderer) {
        mVideoTunnelRenderer->regFillVideoFrameCallBack(NULL, NULL);
        mVideoTunnelRenderer->regNotifyTunnelRenderTimeCallBack(NULL, NULL);

        mVideoTunnelRenderer->stop();
        delete mVideoTunnelRenderer;
        mVideoTunnelRenderer = NULL;
        if (mTunnelHandle) {
            am_gralloc_destroy_sideband_handle(mTunnelHandle);
            mTunnelHandle = NULL;
        }
    }
    return true;
}


bool C2VdecComponent::TunnelHelper::isInResolutionChanging() {
    return (mComp->mGraphicBlocks.size() < mOutBufferCount);
}

void C2VdecComponent::TunnelHelper::onFillVideoFrameTunnel2(int dmafd, bool rendered) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();
    C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Fd:%d, render:%d", __func__, __LINE__, dmafd, rendered);

    struct fillVideoFrame2 frame = {
        .fd = dmafd,
        .rendered = rendered
    };
    mFillVideoFrameQueue.push_back(frame);

    if (!mComp->mCanQueueOutBuffer) {
        C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Cannot queue out buffer, cache it fd:%d, render:%d",
            dmafd, rendered);
        return;
    }

    for (auto &frame : mFillVideoFrameQueue) {
        if (isInResolutionChanging()) {
            C2BlockPool::local_id_t poolId = 0;

            //check fd is new size or old
            mBlockPoolUtil->getPoolId(&poolId);
            auto iter = mOutBufferFdMap.find(dmafd);
            DCHECK(iter != mOutBufferFdMap.end());

            GraphicBlockInfo *info = mComp->getGraphicBlockByFd(frame.fd);
            GraphicBlockInfo *info2 = mComp->getGraphicBlockByBlockId(poolId, iter->second.mBlockId);
            int fd = -1;
            if ((info == NULL) ||
                (info == NULL) ||
                (info != info2)) {
                media::Size& size = mComp->mOutputFormat.mCodedSize;

                if (iter->first >= 0) {
                    close(iter->first);
                }
                mOutBufferFdMap.erase(iter);
                GraphicBlockStateDec(mComp, GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER);
                C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s:%d] Close fd:%d, block id:%d", __func__, __LINE__, iter->first, iter->second.mBlockId);

                allocTunnelBuffer(size, mPixelFormat, &fd);
                GraphicBlockInfo *info = mComp->getGraphicBlockByFd(fd);
                GraphicBlockStateInit(mComp, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
                BufferStatus(mComp, CODEC2_LOG_TAG_BUFFER, "tunnel new allocate fd=%d, index=%d", info->mFd, info->mBlockId);
                mComp->sendOutputBufferToAccelerator(info, true /*ownByAccelerator*/);
                continue;
            }
        }

        GraphicBlockInfo* info = mComp->getGraphicBlockByFd(frame.fd);
        if (!info) {
            C2VdecTMH_LOG(CODEC2_LOG_ERR, "[%s:%d] Cannot get graphicblock according fd:%d", __func__, __LINE__, dmafd);
            mComp->reportError(C2_CORRUPTED);
            return;
        }

        GraphicBlockStateChange(mComp, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
        BufferStatus(mComp, CODEC2_LOG_TAG_BUFFER, "tunnel new allocate fd=%d, index=%d", info->mFd, info->mBlockId);
        mComp->sendOutputBufferToAccelerator(info, true /* ownByAccelerator */);

        /* for drop, need report finished work */
        if (!frame.rendered) {
            auto pendingBuffer = std::find_if(
                    mComp->mPendingBuffersToWork.begin(), mComp->mPendingBuffersToWork.end(),
                    [id = info->mBlockId](const OutputBufferInfo& o) { return o.mBlockId == id;});
            if (pendingBuffer != mComp->mPendingBuffersToWork.end()) {
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

int C2VdecComponent::TunnelHelper::fillVideoFrameCallback2(void* obj, void* args) {
    C2VdecComponent::TunnelHelper* pTunnelHelper = (C2VdecComponent::TunnelHelper*)obj;
    struct fillVideoFrame2* pfillVideoFrame = (struct fillVideoFrame2*)args;

    pTunnelHelper->postFillVideoFrameTunnel2(pfillVideoFrame->fd, pfillVideoFrame->rendered);

    return 0;
}

void C2VdecComponent::TunnelHelper::onNotifyRenderTimeTunnel(struct renderTime rendertime) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    struct renderTime renderTime = {
        .mediaUs = rendertime.mediaUs,
        .renderUs = rendertime.renderUs,
    };
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s:%d] Rendertime:%" PRId64 "", __func__, __LINE__, renderTime.mediaUs);
    sendOutputBufferToWorkTunnel(&renderTime);
}

int C2VdecComponent::TunnelHelper::notifyTunnelRenderTimeCallback(void* obj, void* args) {
    C2VdecComponent::TunnelHelper* pTunnelHelper = (C2VdecComponent::TunnelHelper*)obj;
    struct renderTime* rendertime = (struct renderTime*)args;
    pTunnelHelper->postNotifyRenderTimeTunnel(rendertime);
    return 0;
}

int C2VdecComponent::TunnelHelper::postFillVideoFrameTunnel2(int dmafd, bool rendered) {
    mTaskRunner->PostTask(FROM_HERE,
        ::base::Bind(&C2VdecComponent::TunnelHelper::onFillVideoFrameTunnel2, ::base::Unretained(this),
            dmafd, rendered));
    return 0;
}

int C2VdecComponent::TunnelHelper::postNotifyRenderTimeTunnel(struct renderTime* rendertime) {
    struct renderTime renderTime = {
        .mediaUs = rendertime->mediaUs,
        .renderUs = rendertime->renderUs,
    };
    mTaskRunner->PostTask(FROM_HERE,
        ::base::Bind(&C2VdecComponent::TunnelHelper::onNotifyRenderTimeTunnel, ::base::Unretained(this),
            base::Passed(&renderTime)));
    return 0;
}


c2_status_t C2VdecComponent::TunnelHelper::sendVideoFrameToVideoTunnel(int32_t pictureBufferId, int64_t bitstreamId) {
    DCHECK(mComp != NULL);
    DCHECK(mVideoTunnelRenderer != NULL);
    int64_t timestamp = -1;

    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] PictureId:%d, bitstreamId:%" PRId64 "", __func__, __LINE__,
            pictureBufferId, bitstreamId);
    GraphicBlockInfo* info = mComp->getGraphicBlockById(pictureBufferId);
    if (info->mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR ||
        info->mState == GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER) {
        C2VdecTMH_LOG(CODEC2_LOG_ERR, "Graphic block (id=%d) should not be owned by accelerator or tunnelrender", info->mBlockId);
        return C2_BAD_STATE;
    }

    C2Work* work = mComp->getPendingWorkByBitstreamId(bitstreamId);
    if (!work) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Fd:%d, pts:%" PRId64"", __func__, __LINE__, info->mFd, timestamp);
        return C2_CORRUPTED;
    }
    timestamp = work->input.ordinal.timestamp.peekull();

    // implement Android Video Peek
    if (!mAndroidPeekFrameReady) {
        mAndroidPeekFrameReady = true;
        work = mComp->cloneWork(work);
        std::unique_ptr<C2StreamTunnelHoldRender::output> frameReady = std::make_unique<C2StreamTunnelHoldRender::output>();
        frameReady->value = C2_TRUE;
        work->worklets.front()->output.configUpdate.push_back(std::move(frameReady));
        mComp->sendClonedWork(work, 0);
        C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Send cloned work for FirstFrameReady", __func__, __LINE__);
    }

    if (mVideoTunnelRenderer) {
        GraphicBlockStateChange(mComp, info, GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER);
        BufferStatus(mComp, CODEC2_LOG_TAG_BUFFER, "tunnel send to videotunnel fd=%d, pts=%" PRId64"", info->mFd, timestamp);
        if (mIntfImpl->mVendorNetflixVPeek->vpeek == true) {
            //netflix vpeek need render at once.
            mVideoTunnelRenderer->sendVideoFrame(info->mFd, timestamp, true);
            mIntfImpl->mVendorNetflixVPeek->vpeek = false;
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
    DCHECK(mComp != NULL);
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Rendertime:%" PRId64 "", __func__, __LINE__, rendertime->mediaUs);

    if (mComp->mPendingBuffersToWork.empty() ||
        mComp->mPendingWorks.empty()) {
        C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Empty pendingwork, ignore report it");
        return C2_OK;
    }
    auto nextBuffer = mComp->mPendingBuffersToWork.front();
    if (rendertime->mediaUs < nextBuffer.mMediaTimeUs) {
        C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Old timestamp, ignore report it");
        return C2_OK;
    }

    C2Work* work = mComp->getPendingWorkByMediaTime(rendertime->mediaUs);
    if (!work) {
        for (auto it = mTunnelAbandonMediaTimeQueue.begin(); it != mTunnelAbandonMediaTimeQueue.end(); it++) {
            if (rendertime->mediaUs == *it) {
                mTunnelAbandonMediaTimeQueue.erase(it);
                CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Not find the correct work with mediaTime:%" PRId64", correct work have abandoned and report to framework", rendertime->mediaUs);
                mComp->erasePendingBuffersToWorkByTime(rendertime->mediaUs);
                return C2_OK;
            }
        }

        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "not found corresponed work with mediaTime:%lld", (long long)rendertime->mediaUs);
        mComp->erasePendingBuffersToWorkByTime(rendertime->mediaUs);
        return C2_OK;
    }

    mIntfImpl->mTunnelSystemTimeOut->value = rendertime->renderUs * 1000;
    work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*(mIntfImpl->mTunnelSystemTimeOut)));

    auto pendingBuffer = mComp->findPendingBuffersToWorkByTime(rendertime->mediaUs);
    if (pendingBuffer != mComp->mPendingBuffersToWork.end()) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Rendertime:%" PRId64 ", bitstreamId:%d, flags:%d", __func__, __LINE__, rendertime->mediaUs, pendingBuffer->mBitstreamId,pendingBuffer->flags);
        mComp->reportWorkIfFinished(pendingBuffer->mBitstreamId,pendingBuffer->flags);
        mComp->mPendingBuffersToWork.erase(pendingBuffer);
        /* EOS work check */
        if ((mComp->mPendingWorks.size() == 1u) &&
            mComp->mPendingOutputEOS) {
            C2Work* eosWork = mComp->mPendingWorks.front().get();
            DCHECK((eosWork->input.flags & C2FrameData::FLAG_END_OF_STREAM) > 0);
            mIntfImpl->mTunnelSystemTimeOut->value = systemTime(SYSTEM_TIME_MONOTONIC);
            eosWork->worklets.front()->output.ordinal.timestamp = INT64_MAX;
            eosWork->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*(mIntfImpl->mTunnelSystemTimeOut)));
            C2VdecTMH_LOG(CODEC2_LOG_INFO, "[%s:%d] eos work report", __func__, __LINE__);
            mComp->reportEOSWork();
        }
    }

    return C2_OK;
}


c2_status_t C2VdecComponent::TunnelHelper::storeAbandonedFrame(int64_t timeus) {
    mTunnelAbandonMediaTimeQueue.push_back(timeus);

    return C2_OK;
}

c2_status_t C2VdecComponent::TunnelHelper::allocTunnelBuffersAndSendToDecoder(const media::Size& size, uint32_t pixelFormat) {
    mBlockPoolUtil = mComp->mBlockPoolUtil.get();
    mPixelFormat = pixelFormat;
    mOutBufferCount = mComp->mOutBufferCount;
    DCHECK(mBlockPoolUtil != NULL);

    mBlockPoolUtil->requestNewBufferSet(mOutBufferCount);
    if (mComp->mVideoDecWraper) {
        mComp->mVideoDecWraper->assignPictureBuffers(mOutBufferCount);
        mComp->mCanQueueOutBuffer = true;
    }

    allocTunnelBufferAndSendToDecoder(size, pixelFormat, 0);
    return C2_OK;
}

c2_status_t C2VdecComponent::TunnelHelper::allocTunnelBuffer(const media::Size& size, uint32_t pixelFormat, int* pFd) {
    uint64_t platformUsage = getPlatformUsage();
    C2MemoryUsage usage = {
            mSecure ? (C2MemoryUsage::READ_PROTECTED | C2MemoryUsage::WRITE_PROTECTED) :
            (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE),  platformUsage};

    C2BlockPool::local_id_t poolId = -1;
    uint32_t blockId = -1;
    int fd = -1;
    std::shared_ptr<C2GraphicBlock> c2Block;
    auto err = C2_TIMED_OUT;

    mBlockPoolUtil->getPoolId(&poolId);
    err = mBlockPoolUtil->fetchGraphicBlock(size.width(), size.height(), pixelFormat, usage, &c2Block);
    if (err != C2_OK) {
        C2VdecTMH_LOG(CODEC2_LOG_ERR, "[%s] alloc buffer failed, please check!", __func__);
    } else {
        err = mBlockPoolUtil->getBlockIdByGraphicBlock(c2Block, &blockId);
        if (err != C2_OK) {
            C2VdecTMH_LOG(CODEC2_LOG_ERR, "[%s] get the block id failed, please check!", __func__);
            return C2_BAD_VALUE;
        }
        mBlockPoolUtil->getBlockFd(c2Block, &fd);

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
    C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL2, "%s#%d create buffer#%d", __func__, __LINE__, index);
    if (index >= mOutBufferCount || mIntfImpl == NULL) {
        return;
    }

    int fd = -1;
    allocTunnelBuffer(size, pixelFormat, &fd);
    GraphicBlockInfo* info = mComp->getGraphicBlockByFd(fd);
    GraphicBlockStateInit(mComp, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
    BufferStatus(mComp, CODEC2_LOG_TAG_BUFFER, "tunnel allocate fd=%d, index=%d", info->mFd, info->mBlockId);

    mComp->sendOutputBufferToAccelerator(info, true);
    mTaskRunner->PostTask(FROM_HERE,
        ::base::Bind(&C2VdecComponent::TunnelHelper::allocTunnelBufferAndSendToDecoder, ::base::Unretained(this),
        size, pixelFormat, index+1));

    return;
}

c2_status_t C2VdecComponent::TunnelHelper::videoResolutionChangeTunnel() {
    bool sizeChanged = false;
    bool bufferNumLarged = false;
    bool bufferNumSet = false;
    uint64_t platformUsage = getPlatformUsage();
    C2MemoryUsage usage = {
            mSecure ? (C2MemoryUsage::READ_PROTECTED | C2MemoryUsage::WRITE_PROTECTED) :
            (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE),  platformUsage};
    media::Size& size = mComp->mOutputFormat.mCodedSize;
    mComp->mPendingOutputFormat.reset();


    C2VdecTMH_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
    for (auto& info : mComp->mGraphicBlocks) {
        info.mFdHaveSet = false;
    }

    C2VdecTMH_LOG(CODEC2_LOG_INFO, "[%s:%d] in resolution changing:%d", __func__, __LINE__, isInResolutionChanging());
    if (checkReallocOutputBuffer(mComp->mLastOutputFormat, mComp->mOutputFormat, &sizeChanged, &bufferNumLarged)) {
        if (mReallocWhenResChange && sizeChanged) {
            //all realloc buffer
            int alloc_first = 0;
            int fd = -1;
            for (auto& info : mComp->mGraphicBlocks) {
                if (info.mState != GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER) {
                    GraphicBlockInfo* info1 = &info;
                    GraphicBlockStateChange(mComp, info1, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
                    auto iter = mOutBufferFdMap.find(info.mFd);
                    if (iter == mOutBufferFdMap.end()) {
                        //error
                    }
                    close(iter->first);
                    mOutBufferFdMap.erase(iter);
                    C2VdecTMH_LOG(CODEC2_LOG_INFO, " [%s:%d] close fd:%d, blockid:%d", __func__, __LINE__, iter->first, iter->second.mBlockId);
                    alloc_first ++;
                    GraphicBlockStateReset(mComp, info1);
                }
                info.mGraphicBlock.reset();
            }
            mComp->mGraphicBlocks.clear();
            resetBlockPoolBuffers();
            mBlockPoolUtil->requestNewBufferSet(mOutBufferCount);
            bufferNumSet = true;
            for (int i = 0; i < alloc_first; i++) {
                allocTunnelBuffer(size, mPixelFormat, &fd);
                GraphicBlockInfo* info = mComp->getGraphicBlockByFd(fd);
                GraphicBlockStateInit(mComp, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
                BufferStatus(mComp, CODEC2_LOG_TAG_BUFFER, "tunnel new allocate fd=%d, index=%d", info->mFd, info->mBlockId);
            }
        } else if (bufferNumLarged) {
            //add new allocate buffer
            for (auto& info : mComp->mGraphicBlocks) {
                info.mFdHaveSet = false;
            }
            int32_t lastBufferCount = mComp->mLastOutputFormat.mMinNumBuffers;
            mOutBufferCount = mComp->mOutputFormat.mMinNumBuffers;

            int fd = -1;
            for (int i = lastBufferCount; i < mOutBufferCount; i++) {
                allocTunnelBuffer(size, mPixelFormat, &fd);
                GraphicBlockInfo* info = mComp->getGraphicBlockByFd(fd);
                GraphicBlockStateInit(mComp, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
                BufferStatus(mComp, CODEC2_LOG_TAG_BUFFER, "tunnel new allocate fd=%d, index=%d", info->mFd, info->mBlockId);
            }
        }
    }

    if (!bufferNumSet) {
        mBlockPoolUtil->requestNewBufferSet(mOutBufferCount);
    }
    if (mComp->mVideoDecWraper) {
        mComp->mVideoDecWraper->assignPictureBuffers(mOutBufferCount);
        mComp->mCanQueueOutBuffer = true;
    }

    for (auto& info : mComp->mGraphicBlocks) {
        if (info.mState == GraphicBlockInfo::State::OWNED_BY_COMPONENT) {
            mComp->sendOutputBufferToAccelerator(&info, true);
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
    mComp->resetBlockPoolUtil();
    mBlockPoolUtil = mComp->mBlockPoolUtil.get();
    return C2_OK;
}


bool C2VdecComponent::TunnelHelper::checkReallocOutputBuffer(VideoFormat video_format_old,
                VideoFormat video_format_new,
                bool *sizeChanged, bool *bufferNumLarged) {
    bool bufferNumEnlarged = false;
    bool frameSizeChanged = false;

    C2VdecTMH_LOG(CODEC2_LOG_INFO, "[%s] %dx%d(%d)->%dx%d(%d)", __func__,
                 video_format_old.mCodedSize.width(),
                 video_format_old.mCodedSize.height(),
                 video_format_old.mMinNumBuffers,
                 video_format_new.mCodedSize.width(),
                 video_format_new.mCodedSize.height(),
                 video_format_new.mMinNumBuffers);
    if (video_format_new.mMinNumBuffers > video_format_old.mMinNumBuffers) {
        bufferNumEnlarged = true;
        C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Buffer num larged");
    }
    if (video_format_new.mCodedSize.width() != video_format_old.mCodedSize.width() ||
        video_format_new.mCodedSize.height() !=  video_format_old.mCodedSize.height()) {
        frameSizeChanged = true;
        C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Frame size changed");
    }

    *sizeChanged = frameSizeChanged;
    *bufferNumLarged = bufferNumEnlarged;

    if (bufferNumEnlarged || frameSizeChanged) {
        return true;
    }

    return false;
}

void C2VdecComponent::TunnelHelper::appendTunnelOutputBuffer(std::shared_ptr<C2GraphicBlock> block, int fd, uint32_t blockId, uint32_t poolId) {
    DCHECK(mComp != NULL);
    C2VdecComponent::GraphicBlockInfo info;

    info.mGraphicBlock = std::move(block);
    info.mBlockId = blockId;
    info.mPoolId = poolId;
    info.mFd = fd;
    info.mNeedRealloc = false;
    info.mFdHaveSet = false;
    mComp->mGraphicBlocks.push_back(std::move(info));
}

uint64_t C2VdecComponent::TunnelHelper::getPlatformUsage() {
    uint64_t usage = am_gralloc_get_video_decoder_full_buffer_usage();

    switch (mIntfImpl->getInputCodec()) {
        case InputCodec::H264:
        case InputCodec::MP2V:
        case InputCodec::MP4V:
        case InputCodec::DVAV:
        case InputCodec::MJPG:
            //C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL2, "1:1 usage");
            usage = am_gralloc_get_video_decoder_full_buffer_usage();
            break;
        case InputCodec::H265:
        case InputCodec::VP9:
        case InputCodec::AV1:
        case InputCodec::DVHE:
        case InputCodec::DVAV1:
        default:
            //C2VdecTMH_LOG(CODEC2_LOG_DEBUG_LEVEL2, "1:16 usage");
            usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
            break;
    }

    /* check debug doublewrite */
    char value[PROPERTY_VALUE_MAX];
    if (property_get(C2_PROPERTY_VDEC_DOUBLEWRITE, value, NULL) > 0) {
        int32_t doublewrite_debug = atoi(value);
        C2VdecTMH_LOG(CODEC2_LOG_INFO, "Set double:%d", doublewrite_debug);
        if (doublewrite_debug != 0) {
            switch (doublewrite_debug) {
                case 1:
                case 0x10:
                    usage = am_gralloc_get_video_decoder_full_buffer_usage();
                    break;
                case 2:
                case 3:
                    usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
                    break;
                case 4:
                    usage = am_gralloc_get_video_decoder_quarter_buffer_usage();
                    break;
                default:
                    usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
                    break;
            }
        }
    }

    return usage & C2MemoryUsage::PLATFORM_MASK;
}

}
