#define LOG_NDEBUG 0

#define LOG_TAG "TunnelModeHelper"

#include <base/bind.h>
#include <base/bind_helpers.h>
#include <C2AllocatorGralloc.h>

#include <am_gralloc_ext.h>

#include <logdebug.h>
#include <C2VDATunnelModeHelper.h>
#include <C2VDAInterfaceImpl.h>


#define C2VDATMH_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, C2VDAComponent::mInstanceID, mComp->mCurInstanceID, ##str)

namespace android {

constexpr uint32_t kTunnelModeMediaTimeQueueMax = 16;   // Max queue size for tunnel mode render time.

#define RETURN_ON_UNINITIALIZED_OR_ERROR()                                 \
    do {                                                                   \
        if (mComp->mHasError \
            || mComp->mComponentState == ComponentState::UNINITIALIZED \
            || mComp->mComponentState == ComponentState::DESTROYING \
            || mComp->mComponentState == ComponentState::DESTROYED) \
            return;                                                        \
    } while (0)


C2VDAComponent::TunnelModeHelper::TunnelModeHelper(C2VDAComponent* comp, bool secure):
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
    mReallocWhenResChange = property_get_bool("vendor.media.c2.vdec.realloc_for_tunnel_reschange", mReallocWhenResChange);

    if (mVideoTunnelRenderer) {
        mTunnelId = mVideoTunnelRenderer->getTunnelId();
        mTunnelHandle = am_gralloc_create_sideband_handle(AM_FIXED_TUNNEL, mTunnelId);
        if (mTunnelHandle) {
            CHECK_EQ(mIntfImpl->mTunnelHandleOutput->flexCount(), mTunnelHandle->numInts);
            memcpy(mIntfImpl->mTunnelHandleOutput->m.values, &mTunnelHandle->data[mTunnelHandle->numFds], sizeof(int32_t) * mTunnelHandle->numInts);
        }
    }
    mAndroidPeekFrameReady = false;
    propGetInt(CODEC2_LOGDEBUG_PROPERTY, &gloglevel);
    C2VDATMH_LOG(CODEC2_LOG_INFO, "%s:%d", __func__, __LINE__);
}

C2VDAComponent::TunnelModeHelper::~TunnelModeHelper() {
    if (mVideoTunnelRenderer) {
        delete mVideoTunnelRenderer;
        mVideoTunnelRenderer = NULL;
    }
    mTaskRunner = NULL;
    mIntfImpl = NULL;
    C2VDATMH_LOG(CODEC2_LOG_INFO, "%s:%d", __func__, __LINE__);
}

bool C2VDAComponent::TunnelModeHelper::start() {
    if (mVideoTunnelRenderer) {
        if (mVideoTunnelRenderer->init(mSyncId) == false) {
           C2VDATMH_LOG(CODEC2_LOG_ERR, "tunnelrender init failed");
        }
        mVideoTunnelRenderer->regFillVideoFrameCallBack(fillVideoFrameCallback2, this);
        mVideoTunnelRenderer->regNotifyTunnelRenderTimeCallBack(notifyTunnelRenderTimeCallback, this);
        mVideoTunnelRenderer->start();
    }
    //mTunnelBufferUtil = mComp->mTunnelBufferUtil.get();

    return true;
}

bool C2VDAComponent::TunnelModeHelper::stop() {
    mTunnelAbandonMediaTimeQueue.clear();

    for (auto iter = mOutBufferFdMap.begin(); iter != mOutBufferFdMap.end(); iter++) {
        if (iter->first >= 0) {
            close(iter->first);
        }
    }
    mOutBufferFdMap.clear();
    mAndroidPeekFrameReady = false;

    if (mVideoTunnelRenderer) {
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


bool C2VDAComponent::TunnelModeHelper::isInResolutionChanging() {
    return (mComp->mGraphicBlocks.size() < mOutBufferCount);
}

void C2VDAComponent::TunnelModeHelper::onFillVideoFrameTunnelMode2(int dmafd, bool rendered) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();
    C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s:%d fd:%d, render:%d", __func__, __LINE__, dmafd, rendered);

    struct fillVideoFrame2 frame = {
        .fd = dmafd,
        .rendered = rendered
    };
    mFillVideoFrameQueue.push_back(frame);

    if (!mComp->mCanQueueOutBuffer) {
        C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "cannot queue out buffer, cache it fd:%d, render:%d",
            dmafd, rendered);
        return;
    }

    for (auto &frame : mFillVideoFrameQueue) {
        if (isInResolutionChanging()) {
            media::Size& size = mComp->mOutputFormat.mCodedSize;
            auto iter = mOutBufferFdMap.find(dmafd);
            DCHECK(iter != mOutBufferFdMap.end());
            if (iter->first) {
                close(iter->first);
            }
            mOutBufferFdMap.erase(iter);
            allocTunnelBuffer(size, mPixelFormat);
            return;
        }

        GraphicBlockInfo* info = mComp->getGraphicBlockByFd(frame.fd);
        if (!info) {
            C2VDATMH_LOG(CODEC2_LOG_ERR, "%s:%d cannot get graphicblock according fd:%d", __func__, __LINE__, dmafd);
            mComp->reportError(C2_CORRUPTED);
            return;
        }
#if 0
        if (info->mNeedRealloc &&
            (info->mFdHaveSet == false)) {
            //mTunnelBufferUtil->freeTunnelBuffer(info->mFd);
            int shardfd = -1;
            uint64_t platformUsage = getPlatformUsage();
            media::Size& size = mComp->mOutputFormat.mCodedSize;
            C2MemoryUsage usage = {
                    mSecure ? (C2MemoryUsage::READ_PROTECTED | C2MemoryUsage::WRITE_PROTECTED) :
                    (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE),  platformUsage};
            //bool ret = mTunnelBufferUtil->fetchTunnelBuffer(size.width(), size.height(),
                                                    //mPixelFormat, usage,  &shardfd);
            if (ret < 0) {
                //alloc buffer from uvm failed
                C2VDATMH_LOG(CODEC2_LOG_ERR, " %s:%d alloc buffer from uvm failed, please check!", __func__, __LINE__);
            } else {
                DCHECK(shardfd >= 0);
                C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1,"%s:%d %dx%d, fd:%d", __func__, __LINE__,
                             size.width(), size.height(), shardfd);
                info->mFd = shardfd;
                info->mNeedRealloc = false;
            }
        }
#endif

        info->mState = GraphicBlockInfo::State::OWNED_BY_COMPONENT;
        mComp->sendOutputBufferToAccelerator(info, true /* ownByAccelerator */);

        /* for drop, need report finished work */
        if (!frame.rendered) {
            auto pendingbuffer = std::find_if(
                    mComp->mPendingBuffersToWork.begin(), mComp->mPendingBuffersToWork.end(),
                    [id = info->mBlockId](const OutputBufferInfo& o) { return o.mBlockId == id;});
            if (pendingbuffer != mComp->mPendingBuffersToWork.end()) {
                struct VideoTunnelRendererWraper::renderTime rendertime = {
                    .mediaUs = pendingbuffer->mMediaTimeUs,
                    .renderUs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000,
                };
                sendOutputBufferToWorkTunnel(&rendertime);
            }
        }
    }
    mFillVideoFrameQueue.clear();
}

int C2VDAComponent::TunnelModeHelper::fillVideoFrameCallback2(void* obj, void* args) {
    C2VDAComponent::TunnelModeHelper* pTunnelHelper = (C2VDAComponent::TunnelModeHelper*)obj;
    struct fillVideoFrame2* pfillVideoFrame = (struct fillVideoFrame2*)args;

    pTunnelHelper->postFillVideoFrameTunnelMode2(pfillVideoFrame->fd, pfillVideoFrame->rendered);

    return 0;
}

void C2VDAComponent::TunnelModeHelper::onNotifyRenderTimeTunnelMode(struct VideoTunnelRendererWraper::renderTime rendertime) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    struct VideoTunnelRendererWraper::renderTime renderTime = {
        .mediaUs = rendertime.mediaUs,
        .renderUs = rendertime.renderUs,
    };
    C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL2, "%s:%d rendertime:%lld", __func__, __LINE__, renderTime.mediaUs);
    sendOutputBufferToWorkTunnel(&renderTime);
}

int C2VDAComponent::TunnelModeHelper::notifyTunnelRenderTimeCallback(void* obj, void* args) {
    C2VDAComponent::TunnelModeHelper* pTunnelHelper = (C2VDAComponent::TunnelModeHelper*)obj;
    struct VideoTunnelRendererWraper::renderTime* rendertime = (struct VideoTunnelRendererWraper::renderTime*)args;
    pTunnelHelper->postNotifyRenderTimeTunnelMode(rendertime);
    return 0;
}

int C2VDAComponent::TunnelModeHelper::postFillVideoFrameTunnelMode2(int dmafd, bool rendered) {
    mTaskRunner->PostTask(FROM_HERE,
        ::base::Bind(&C2VDAComponent::TunnelModeHelper::onFillVideoFrameTunnelMode2, ::base::Unretained(this),
            dmafd, rendered));
    return 0;
}

int C2VDAComponent::TunnelModeHelper::postNotifyRenderTimeTunnelMode(struct VideoTunnelRendererWraper::renderTime* rendertime) {
    struct VideoTunnelRendererWraper::renderTime renderTime = {
        .mediaUs = rendertime->mediaUs,
        .renderUs = rendertime->renderUs,
    };
    mTaskRunner->PostTask(FROM_HERE,
        ::base::Bind(&C2VDAComponent::TunnelModeHelper::onNotifyRenderTimeTunnelMode, ::base::Unretained(this),
            base::Passed(&renderTime)));
    return 0;
}


c2_status_t C2VDAComponent::TunnelModeHelper::sendVideoFrameToVideoTunnel(int32_t pictureBufferId, int64_t bitstreamId) {
    DCHECK(mComp != NULL);
    DCHECK(mVideoTunnelRenderer != NULL);
    int64_t timestamp = -1;

    C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s:%d pictureId:%d, bitstreamId:%lld", __func__, __LINE__,
            pictureBufferId, bitstreamId);
    GraphicBlockInfo* info = mComp->getGraphicBlockById(pictureBufferId);
    if (info->mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR ||
        info->mState == GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER) {
        C2VDATMH_LOG(CODEC2_LOG_ERR, "Graphic block (id=%d) should not be owned by accelerator or tunnelrender", info->mBlockId);
        return C2_BAD_STATE;
    }

    C2Work* work = mComp->getPendingWorkByBitstreamId(bitstreamId);
    if (!work) {
        C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s:%d fd:%d, pts:%lld", __func__, __LINE__, info->mFd, timestamp);
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
        C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s:%d send cloned work for FirstFrameReady", __func__, __LINE__);
    }

    if (mVideoTunnelRenderer) {
        C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s:%d fd:%d, pts:%lld", __func__, __LINE__, info->mFd, timestamp);
        info->mState = GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER;
        mVideoTunnelRenderer->sendVideoFrame(info->mFd, timestamp);
    }

    return C2_OK;
}

c2_status_t C2VDAComponent::TunnelModeHelper::flush() {
    if (mVideoTunnelRenderer) {
        mVideoTunnelRenderer->flush();
    }

    return C2_OK;
}

c2_status_t C2VDAComponent::TunnelModeHelper::sendOutputBufferToWorkTunnel(struct VideoTunnelRendererWraper::renderTime* rendertime) {
    DCHECK(mComp != NULL);
    C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s:%d rendertime:%lld", __func__, __LINE__, rendertime->mediaUs);

    if (mComp->mPendingBuffersToWork.empty() ||
        mComp->mPendingWorks.empty()) {
        C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "empty pendingwork, ignore report it");
        return C2_OK;
    }
    auto nextBuffer = mComp->mPendingBuffersToWork.front();
    if (rendertime->mediaUs < nextBuffer.mMediaTimeUs) {
        C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "old timestamp, ignore report it");
        return C2_OK;
    }

    C2Work* work = mComp->getPendingWorkByMediaTime(rendertime->mediaUs);
    if (!work) {
        for (auto it = mTunnelAbandonMediaTimeQueue.begin(); it != mTunnelAbandonMediaTimeQueue.end(); it++) {
            if (rendertime->mediaUs == *it) {
                mTunnelAbandonMediaTimeQueue.erase(it);
                C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL2, "not find the correct work with mediaTime:%lld, correct work have abandoed and report to framework", rendertime->mediaUs);
                mComp->erasePendingBuffersToWorkByTime(rendertime->mediaUs);
                return C2_OK;
            }
        }

        if (/*mMetaDataUtil->isInterlaced()
            && (mComp->mInterlacedType == (C2_INTERLACED_TYPE_SETUP | C2_INTERLACED_TYPE_2FIELD))*/0) {
            auto time = std::find_if(mTunnelRenderMediaTimeQueue.begin(), mTunnelRenderMediaTimeQueue.end(),
                    [timeus=rendertime->mediaUs](const int64_t _timeus) {return _timeus == timeus;});
            if (time != mTunnelRenderMediaTimeQueue.end()) {
                C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL2, "have report mediaTime:%lld, ignore it", rendertime->mediaUs);
                mComp->erasePendingBuffersToWorkByTime(rendertime->mediaUs);
                return C2_OK;
            }
        }

        C2VDATMH_LOG(CODEC2_LOG_ERR, "not find the correct work with mediaTime:%lld, should have reported, discard report it", rendertime->mediaUs);
        mComp->erasePendingBuffersToWorkByTime(rendertime->mediaUs);
        return C2_OK;
    }
    if (/*mMetaDataUtil->isInterlaced()
        && (mComp->mInterlacedType == (C2_INTERLACED_TYPE_SETUP | C2_INTERLACED_TYPE_2FIELD))*/0) {
        auto time = std::find_if(mTunnelRenderMediaTimeQueue.begin(), mTunnelRenderMediaTimeQueue.end(),
                [timeus=rendertime->mediaUs](const int64_t _timeus) {return _timeus == timeus;});
        if (time == mTunnelRenderMediaTimeQueue.end()) {
            mTunnelRenderMediaTimeQueue.push_back(rendertime->mediaUs);
            C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "tunnelmode push mediaTime:%lld", rendertime->mediaUs);
        }
        if (mTunnelRenderMediaTimeQueue.size() > kTunnelModeMediaTimeQueueMax) {
            mTunnelRenderMediaTimeQueue.pop_front();
        }
    }

    mIntfImpl->mTunnelSystemTimeOut->value = rendertime->renderUs * 1000;
    work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*(mIntfImpl->mTunnelSystemTimeOut)));

    auto pendingbuffer = mComp->findPendingBuffersToWorkByTime(rendertime->mediaUs);
    if (pendingbuffer != mComp->mPendingBuffersToWork.end()) {
        //info->mState = GraphicBlockInfo::State::OWNED_BY_CLIENT;
        C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s:%d rendertime:%lld, bitstreamId:%d, flags:%d", __func__, __LINE__, rendertime->mediaUs, pendingbuffer->mBitstreamId,pendingbuffer->flags);
        mComp->reportWorkIfFinished(pendingbuffer->mBitstreamId,pendingbuffer->flags);
        mComp->mPendingBuffersToWork.erase(pendingbuffer);
        /* EOS work check */
        if ((mComp->mPendingWorks.size() == 1u) &&
            mComp->mPendingOutputEOS) {
            C2Work* eosWork = mComp->mPendingWorks.front().get();
            DCHECK((eosWork->input.flags & C2FrameData::FLAG_END_OF_STREAM) > 0);
            mIntfImpl->mTunnelSystemTimeOut->value = systemTime(SYSTEM_TIME_MONOTONIC);
            eosWork->worklets.front()->output.ordinal.timestamp = INT64_MAX;
            eosWork->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*(mIntfImpl->mTunnelSystemTimeOut)));
            C2VDATMH_LOG(CODEC2_LOG_INFO, "%s:%d eos work report", __func__, __LINE__);
            mComp->reportEOSWork();
        }
    }

    return C2_OK;
}


c2_status_t C2VDAComponent::TunnelModeHelper::storeAbandonedFrame(int64_t timeus) {
    mTunnelAbandonMediaTimeQueue.push_back(timeus);

    return C2_OK;
}

c2_status_t C2VDAComponent::TunnelModeHelper::allocTunnelBuffersAndSendToDecoder(const media::Size& size, uint32_t pixelFormat) {
    mBlockPoolUtil = mComp->mBlockPoolUtil.get();
    mPixelFormat = pixelFormat;
    mOutBufferCount = mComp->mOutBufferCount;
    DCHECK(mBlockPoolUtil != NULL);

    mBlockPoolUtil->requestNewBufferSet(mOutBufferCount);
    if (mComp->mVideoDecWraper) {
        mComp->mVideoDecWraper->assignPictureBuffers(mOutBufferCount);
        mComp->mCanQueueOutBuffer = true;
    }

    for (int i = 0; i < mOutBufferCount; i++) {
        allocTunnelBuffer(size, pixelFormat);
    }

    for (auto& info : mComp->mGraphicBlocks) {
        mComp->sendOutputBufferToAccelerator(&info, true /*ownByAccelerator*/);
    }

    return C2_OK;
}

c2_status_t C2VDAComponent::TunnelModeHelper::allocTunnelBuffer(const media::Size& size, uint32_t pixelFormat) {
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
        C2VDATMH_LOG(CODEC2_LOG_ERR, " %s alloc buffer failed, please check!", __func__);
    } else {
        err = mBlockPoolUtil->getBlockIdByGraphicBlock(c2Block, &blockId);
        mBlockPoolUtil->getBlockFd(c2Block, &fd);
        mComp->appendOutputBuffer(std::move(c2Block), poolId, blockId, true);

        mOutBufferFdMap.insert(std::pair<int, int>(dup(fd), fd));
    }
    C2VDATMH_LOG(CODEC2_LOG_INFO, " %s:%d alloc buffer fd:%d", __func__, __LINE__, fd);

    return C2_OK;
}

c2_status_t C2VDAComponent::TunnelModeHelper::videoResolutionChangeTunnel() {
    bool sizeChanged = false;
    bool bufferNumLarged = false;
    uint64_t platformUsage = getPlatformUsage();
    C2MemoryUsage usage = {
            mSecure ? (C2MemoryUsage::READ_PROTECTED | C2MemoryUsage::WRITE_PROTECTED) :
            (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE),  platformUsage};
    media::Size& size = mComp->mOutputFormat.mCodedSize;
    mComp->mPendingOutputFormat.reset();


    C2VDATMH_LOG(CODEC2_LOG_INFO, " %s:%d", __func__, __LINE__);
    for (auto& info : mComp->mGraphicBlocks) {
        info.mFdHaveSet = false;
    }

    if (checkReallocOutputBuffer(mComp->mLastOutputFormat, mComp->mOutputFormat, &sizeChanged, &bufferNumLarged)) {
        if (mReallocWhenResChange && sizeChanged) {
            //all realloc buffer
            for (auto& info : mComp->mGraphicBlocks) {
                info.mGraphicBlock.reset();
            }
            mComp->mGraphicBlocks.clear();
            resetBlockPoolBuffers();
        } else if (bufferNumLarged) {
            //add new allocate buffer
            for (auto& info : mComp->mGraphicBlocks) {
                info.mFdHaveSet = false;
            }
            int32_t lastbuffercount = mComp->mLastOutputFormat.mMinNumBuffers;
            mOutBufferCount = mComp->mOutputFormat.mMinNumBuffers;

            for (int i = lastbuffercount; i < mOutBufferCount; i++) {
                allocTunnelBuffer(size, mPixelFormat);
            }
        }
    }

    mBlockPoolUtil->requestNewBufferSet(mOutBufferCount);
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

c2_status_t C2VDAComponent::TunnelModeHelper::onAndroidVideoPeek() {
    if (mVideoTunnelRenderer) {
        mVideoTunnelRenderer->peekFirstFrame();
    }

    return C2_OK;
}

c2_status_t C2VDAComponent::TunnelModeHelper::resetBlockPoolBuffers() {
    mBlockPoolUtil->cancelAllGraphicBlock();
    return C2_OK;
}


bool C2VDAComponent::TunnelModeHelper::checkReallocOutputBuffer(VideoFormat video_format_old,
                VideoFormat video_format_new,
                bool *sizeChanged, bool *bufferNumLarged) {
    bool buffernumenlarged = false;
    bool framesizechanged = false;

    C2VDATMH_LOG(CODEC2_LOG_INFO, "%s %dx%d(%d)->%dx%d(%d)", __func__,
                 video_format_old.mCodedSize.width(),
                 video_format_old.mCodedSize.height(),
                 video_format_old.mMinNumBuffers,
                 video_format_new.mCodedSize.width(),
                 video_format_new.mCodedSize.height(),
                 video_format_new.mMinNumBuffers);
    if (video_format_new.mMinNumBuffers > video_format_old.mMinNumBuffers) {
        buffernumenlarged = true;
        C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "buffer num larged");
    }
    if (video_format_new.mCodedSize.width() != video_format_old.mCodedSize.width() ||
        video_format_new.mCodedSize.height() !=  video_format_old.mCodedSize.height()) {
        framesizechanged = true;
        C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL1, "frame size changed");
    }

    *sizeChanged = framesizechanged;
    *bufferNumLarged = buffernumenlarged;

    if (buffernumenlarged || framesizechanged) {
        return true;
    }

    return false;
}

void C2VDAComponent::TunnelModeHelper::appendTunnelOutputBuffer(int fd, uint32_t blockId) {
    DCHECK(mComp != NULL);
    C2VDAComponent::GraphicBlockInfo info;

    info.mBlockId = blockId;
    info.mFd = fd;
    info.mNeedRealloc = false;
    info.mFdHaveSet = false;
    mComp->mGraphicBlocks.push_back(std::move(info));
}

uint64_t C2VDAComponent::TunnelModeHelper::getPlatformUsage() {
    uint64_t usage = am_gralloc_get_video_decoder_full_buffer_usage();

    switch (mIntfImpl->getInputCodec()) {
        case InputCodec::H264:
        case InputCodec::MP2V:
        case InputCodec::MP4V:
        case InputCodec::DVAV:
        case InputCodec::MJPG:
            //C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL2, "1:1 usage");
            usage = am_gralloc_get_video_decoder_full_buffer_usage();
            break;
        case InputCodec::H265:
        case InputCodec::VP9:
        case InputCodec::AV1:
        case InputCodec::DVHE:
        case InputCodec::DVAV1:
        default:
            //C2VDATMH_LOG(CODEC2_LOG_DEBUG_LEVEL2, "1:16 usage");
            usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
            break;
    }

    /* check debug doublewrite */
    char value[PROPERTY_VALUE_MAX];
    if (property_get("vendor.media.doublewrite", value, NULL) > 0) {
        int32_t doublewrite_debug = atoi(value);
        C2VDATMH_LOG(CODEC2_LOG_INFO, "set double:%d", doublewrite_debug);
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
