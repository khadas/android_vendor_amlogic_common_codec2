// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define LOG_NDEBUG 0
#define LOG_TAG "C2VDAComponent"

#if 0
#ifdef V4L2_CODEC2_ARC
#include <C2VDAAdaptorProxy.h>
#else
#include <C2VDAAdaptor.h>
#endif
#endif

#define __C2_GENERATE_GLOBAL_VARS__
//#include <C2ArcSupport.h>  // to getParamReflector from arc store
#include <amuvm.h>
#include <C2VDASupport.h>
#include <C2VDAComponent.h>
//#include <C2VDAPixelFormat.h>
#include <C2Buffer.h>

//#include <h264_parser.h>

#include <C2AllocatorGralloc.h>
#include <C2ComponentFactory.h>
#include <C2PlatformSupport.h>
#include <Codec2Mapper.h>
#include <C2VDAInterfaceImpl.h>

#include <base/bind.h>
#include <base/bind_helpers.h>

#include <android/hardware/graphics/common/1.0/types.h>
#include <cutils/native_handle.h>


#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <ui/GraphicBuffer.h>
#include <utils/Log.h>
#include <utils/misc.h>

#include <inttypes.h>
#include <string.h>
#include <algorithm>
#include <string>

#include <hardware/gralloc1.h>
#include <am_gralloc_ext.h>
#include <cutils/properties.h>

#define ATRACE_TAG ATRACE_TAG_VIDEO

#include <C2VDAMetaDataUtil.h>
#include <C2VDATunnelModeHelper.h>
#include <logdebug.h>

#define ATRACE_TAG ATRACE_TAG_VIDEO
#include <utils/Trace.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <fstream>

#define AM_SIDEBAND_HANDLE_NUM_FD (0)
#define HWSYNCID_PASSTHROUGH_FLAG (1u << 16)

#define CODEC_OUTPUT_BUFS_ALIGN_64 64

#define DEFAULT_FRAME_DURATION (16384)// default dur: 16ms (1 frame at 60fps)
#define DEFAULT_RETRYBLOCK_TIMEOUT_MS (60*1000)// default timeout 1min


#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

#define C2VDA_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, mInstanceID, mCurInstanceID, ##str)

uint32_t android::C2VDAComponent::mDumpFileCnt = 0;
uint32_t android::C2VDAComponent::mInstanceNum = 0;
uint32_t android::C2VDAComponent::mInstanceID = 0;

using android::hardware::graphics::common::V1_0::BufferUsage;

namespace android {

namespace {


// Mask against 30 bits to avoid (undefined) wraparound on signed integer.
int32_t frameIndexToBitstreamId(c2_cntr64_t frameIndex) {
    return static_cast<int32_t>(frameIndex.peeku() & 0x3FFFFFFF);
}

#if 0
// Get android_ycbcr by lockYCbCr() from block handle which uses usage without SW_READ/WRITE bits.
android_ycbcr getGraphicBlockInfo(const C2GraphicBlock& block) {
    uint32_t width, height, format, stride, igbp_slot, generation;
    uint64_t usage, igbp_id;
    android::_UnwrapNativeCodec2GrallocMetadata(block.handle(), &width, &height,
                                                &format, &usage, &stride, &generation, &igbp_id,
                                                &igbp_slot);
    native_handle_t* grallocHandle = android::UnwrapNativeCodec2GrallocHandle(block.handle());
    sp<GraphicBuffer> buf = new GraphicBuffer(grallocHandle, GraphicBuffer::CLONE_HANDLE, width,
                                              height, format, 1, usage, stride);
    native_handle_delete(grallocHandle);

    android_ycbcr ycbcr = {};
    constexpr uint32_t kNonSWLockUsage = 0;
    int32_t status = buf->lockYCbCr(kNonSWLockUsage, &ycbcr);
    if (status != OK)
        ALOGE("lockYCbCr is failed: %d", (int) status);
    buf->unlock();
    return ycbcr;
}
#endif

#if 0
// Get frame size (stride, height) of a buffer owned by |block|.
media::Size getFrameSizeFromC2GraphicBlock(const C2GraphicBlock& block) {
    android_ycbcr ycbcr = getGraphicBlockInfo(block);
    return media::Size(ycbcr.ystride, block.height());
}
#endif

//No-Tunnel Mode
const uint32_t kDpbOutputBufferExtraCount = 0;          // Use the same number as ACodec.
const int kDequeueRetryDelayUs = 10000;                 // Wait time of dequeue buffer retry in microseconds.
const int32_t kAllocateBufferMaxRetries = 10;           // Max retry time for fetchGraphicBlock timeout.
constexpr uint32_t kDefaultSmoothnessFactor = 8;        // Default smoothing margin.(kRenderingDepth + kSmoothnessFactor + 1)

}  // namespace

static c2_status_t adaptorResultToC2Status(VideoDecodeAcceleratorAdaptor::Result result) {
    switch (result) {
    case VideoDecodeAcceleratorAdaptor::Result::SUCCESS:
        return C2_OK;
    case VideoDecodeAcceleratorAdaptor::Result::ILLEGAL_STATE:
        ALOGE("Got error: ILLEGAL_STATE");
        return C2_BAD_STATE;
    case VideoDecodeAcceleratorAdaptor::Result::INVALID_ARGUMENT:
        ALOGE("Got error: INVALID_ARGUMENT");
        return C2_BAD_VALUE;
    case VideoDecodeAcceleratorAdaptor::Result::UNREADABLE_INPUT:
        ALOGE("Got error: UNREADABLE_INPUT");
        return C2_BAD_VALUE;
    case VideoDecodeAcceleratorAdaptor::Result::PLATFORM_FAILURE:
        ALOGE("Got error: PLATFORM_FAILURE");
        return C2_CORRUPTED;
    case VideoDecodeAcceleratorAdaptor::Result::INSUFFICIENT_RESOURCES:
        ALOGE("Got error: INSUFFICIENT_RESOURCES");
        return C2_NO_MEMORY;
    default:
        ALOGE("Unrecognizable adaptor result (value = %d)...", result);
        return C2_CORRUPTED;
    }
}



////////////////////////////////////////////////////////////////////////////////
#define RETURN_ON_UNINITIALIZED_OR_ERROR()                                 \
    do {                                                                   \
        if (mHasError \
            || mComponentState == ComponentState::UNINITIALIZED \
            || mComponentState == ComponentState::DESTROYING \
            || mComponentState == ComponentState::DESTROYED) \
            return;                                                        \
    } while (0)

C2VDAComponent::VideoFormat::VideoFormat(HalPixelFormat pixelFormat, uint32_t minNumBuffers,
                                         media::Size codedSize, media::Rect visibleRect)
      : mPixelFormat(pixelFormat),
        mMinNumBuffers(minNumBuffers),
        mCodedSize(codedSize),
        mVisibleRect(visibleRect) {}


// static
std::atomic<int32_t> C2VDAComponent::sConcurrentInstances = 0;
std::atomic<int32_t> C2VDAComponent::sConcurrentInstanceSecures = 0;

// static
std::shared_ptr<C2Component> C2VDAComponent::create(
        const std::string& name, c2_node_id_t id, const std::shared_ptr<C2ReflectorHelper>& helper,
        C2ComponentFactory::ComponentDeleter deleter) {
    UNUSED(deleter);
    static const int32_t kMaxConcurrentInstances =
            property_get_int32("vendor.codec2.decode.concurrent-instances", 9);
    static const int32_t kMaxSecureConcurrentInstances =
            property_get_int32("vendor.codec2.securedecode.concurrent-instances", 2);
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    bool isSecure = name.find(".secure") != std::string::npos;
    if (isSecure) {
        if (kMaxSecureConcurrentInstances >= 0 && sConcurrentInstanceSecures.load() >= kMaxSecureConcurrentInstances) {
            ALOGW("Reject to Initialize() due to too many secure instances: %d", sConcurrentInstanceSecures.load());
            return nullptr;
        }
    } else {
        if (kMaxConcurrentInstances >= 0 && sConcurrentInstances.load() >= kMaxConcurrentInstances) {
            ALOGW("Reject to Initialize() due to too many instances: %d", sConcurrentInstances.load());
            return nullptr;
        }
    }
    return std::shared_ptr<C2Component>(new C2VDAComponent(name, id, helper));
}

struct DummyReadView : public C2ReadView {
    DummyReadView() : C2ReadView(C2_NO_INIT) {}
};

C2VDAComponent::C2VDAComponent(C2String name, c2_node_id_t id,
                               const std::shared_ptr<C2ReflectorHelper>& helper)
      : mIntfImpl(std::make_shared<IntfImpl>(name, helper)),
        mIntf(std::make_shared<SimpleInterface<IntfImpl>>(name.c_str(), id, mIntfImpl)),
        mThread("C2VDAComponentThread"),
        mDequeueThread("C2VDAComponentDequeueThread"),
        mVDAInitResult(VideoDecodeAcceleratorAdaptor::Result::ILLEGAL_STATE),
        mComponentState(ComponentState::UNINITIALIZED),
        mPendingOutputEOS(false),
        mCodecProfile(media::VIDEO_CODEC_PROFILE_UNKNOWN),
        mState(State::UNLOADED),
        mWeakThisFactory(this),
        mOutputDelay(nullptr),
        mDumpYuvFp(NULL),
        mVideoDecWraper(NULL),
        mMetaDataUtil(NULL),
        mUseBufferQueue(false),
        mBufferFirstAllocated(false),
        mPictureSizeChanged(false),
        mResChStat(C2_RESOLUTION_CHANGE_NONE),
        mSurfaceUsageGeted(false),
        mVDAComponentStopDone(false),
        mCanQueueOutBuffer(false),
        mHDR10PlusMeteDataNeedCheck(false),
        mInputWorkCount(0),
        mInputCSDWorkCount(0),
        mOutputWorkCount(0),
        mSyncType(C2_SYNC_TYPE_NON_TUNNEL),
        mTunerPassthrough(NULL),
        mDefaultDummyReadView(DummyReadView()),
        mInterlacedType(C2_INTERLACED_TYPE_NONE),
        mFirstInputTimestamp(-1),
        mLastOutputBitstreamId(-1),
        mLastFinishedBitstreamId(-1),
        mNeedFinishFirstWork4Interlaced(false),
        mOutPutInfo4WorkIncomplete(NULL),
        mHasQueuedWork(false) {
    ALOGI("%s(%s)", __func__, name.c_str());

    mSecureMode = name.find(".secure") != std::string::npos;
    if (mSecureMode)
        sConcurrentInstanceSecures.fetch_add(1, std::memory_order_relaxed);
    else
        sConcurrentInstances.fetch_add(1, std::memory_order_relaxed);

    mIsDolbyVision = name.find(".dolby-vision") != std::string::npos;

   // TODO(johnylin): the client may need to know if init is failed.
    if (mIntfImpl->status() != C2_OK) {
        ALOGE("Component interface init failed (err code = %d)", mIntfImpl->status());
        return;
    }
    mIntfImpl->setComponent(this);

    if (!mThread.Start()) {
        ALOGE("Component thread failed to start.");
        return;
    }
    mTaskRunner = mThread.task_runner();
    mState.store(State::LOADED);
    mCurInstanceID = mInstanceID;
    mInstanceNum ++;
    mInstanceID ++;
    mUpdateDurationUsCount = 0;

    propGetInt(CODEC2_LOGDEBUG_PROPERTY, &gloglevel);
    C2VDA_LOG(CODEC2_LOG_ERR, "[%s:%d]", __func__, __LINE__);
    bool dump = property_get_bool("vendor.media.codec2.dumpyuv", false);
    if (dump) {
        char pathfile[1024] = { '\0'  };
        sprintf(pathfile, "/data/tmp/codec2_%d.yuv", mDumpFileCnt++);
        mDumpYuvFp = fopen(pathfile, "wb");
        if (mDumpYuvFp) {
            ALOGV("open file %s", pathfile);
        } else {
            ALOGV("open file %s error:%s", pathfile, strerror(errno));
        }
    }
    //default 1min
    mDefaultRetryBlockTimeOutMs = (uint64_t)property_get_int32("vendor.codec2.default.retryblock.timeout", DEFAULT_RETRYBLOCK_TIMEOUT_MS);
    mFdInfoDebugEnable =  property_get_bool("debug.vendor.media.codec2.vdec.fd_info_debug", false);

    if (mFdInfoDebugEnable) {
        getCurrentProcessFdInfo();
    }
}

C2VDAComponent::~C2VDAComponent() {
    ALOGI("%s", __func__);
    mComponentState = ComponentState::DESTROYING;

    if (mFdInfoDebugEnable) {
        getCurrentProcessFdInfo();
    }

    if (mThread.IsRunning()) {
        mTaskRunner->PostTask(FROM_HERE,
                              ::base::Bind(&C2VDAComponent::onDestroy, ::base::Unretained(this)));
        mThread.Stop();
    }
    if (mSecureMode)
        sConcurrentInstanceSecures.fetch_sub(1, std::memory_order_relaxed);
    else
        sConcurrentInstances.fetch_sub(1, std::memory_order_relaxed);
    ALOGI("%s done", __func__);
    --mInstanceNum;
}

void C2VDAComponent::onDestroy() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());

    mPendingOutputFormat.reset();
    mPendingBuffersToWork.clear();
    stopDequeueThread();

    if (mVideoDecWraper) {
        mVideoDecWraper->destroy();
        mVideoDecWraper.reset();
        mVideoDecWraper = NULL;
        if (mMetaDataUtil) {
            mMetaDataUtil.reset();
            mMetaDataUtil = NULL;
        }
        if (mDumpYuvFp)
            fclose(mDumpYuvFp);
    }
    if (mTunnelHelper) {
        mTunnelHelper.reset();
        mTunnelHelper = NULL;
    }

    displayGraphicBlockInfo();
    for (auto& info : mGraphicBlocks) {
        info.mGraphicBlock.reset();
    }

    mGraphicBlocks.clear();
    if (mBlockPoolUtil != NULL) {
        mBlockPoolUtil->cancelAllGraphicBlock();
        mBlockPoolUtil.reset();
        mBlockPoolUtil = NULL;
    }

    mComponentState = ComponentState::DESTROYED;
    ALOGV("onDestroy");
}

void C2VDAComponent::onStart(media::VideoCodecProfile profile, ::base::WaitableEvent* done) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onStart DolbyVision:%d",mIsDolbyVision);
    CHECK_EQ(mComponentState, ComponentState::UNINITIALIZED);

    if (!isTunnerPassthroughMode()) {
        mVideoDecWraper = std::make_shared<VideoDecWraper>();
        mMetaDataUtil =  std::make_shared<MetaDataUtil>(this, mSecureMode);
        mMetaDataUtil->setHDRStaticColorAspects(GetIntfImpl()->getColorAspects());
        mMetaDataUtil->codecConfig(&mConfigParam);

        //update profile for DolbyVision
        if (mIsDolbyVision) {
            media::VideoDecodeAccelerator::SupportedProfiles supportedProfiles;
            InputCodec codec = mIntfImpl->getInputCodec();
            supportedProfiles = VideoDecWraper::AmVideoDec_getSupportedProfiles((uint32_t)codec);
            if (supportedProfiles.empty()) {
                ALOGE("No supported profile from input codec: %d", mIntfImpl->getInputCodec());
                return;
            }
            mCodecProfile = supportedProfiles[0].profile;
            mIntfImpl->updateCodecProfile(mCodecProfile);
            ALOGD("update profile(%d) to (%d) mime(%s)", profile, mCodecProfile, VideoCodecProfileToMime(mCodecProfile));
            profile = mCodecProfile;
        }
        uint32_t vdecflags = AM_VIDEO_DEC_INIT_FLAG_CODEC2;
        if (mIntfImpl) {
            if (mIntfImpl->mVdecWorkMode->value == VDEC_STREAMMODE)
                vdecflags |= AM_VIDEO_DEC_INIT_FLAG_STREAMMODE;
            if (mIntfImpl->mDataSourceType->value == DATASOURCE_DMX)
                vdecflags |= AM_VIDEO_DEC_INIT_FLAG_DMXDATA_SOURCE;
        }
        mVDAInitResult = (VideoDecodeAcceleratorAdaptor::Result)mVideoDecWraper->initialize(VideoCodecProfileToMime(profile),
                (uint8_t*)&mConfigParam, sizeof(mConfigParam), mSecureMode, this, vdecflags);
    } else {
        mVDAInitResult = VideoDecodeAcceleratorAdaptor::Result::SUCCESS;
    }

    if (isTunnelMode() && mTunnelHelper) {
        mTunnelHelper->start();
    }

    if (!mSecureMode && (mIntfImpl->getInputCodec() == InputCodec::H264
                || mIntfImpl->getInputCodec() == InputCodec::H265
                || mIntfImpl->getInputCodec() == InputCodec::MP2V)) {
        // Get default color aspects on start.
        updateColorAspects();
    }

    if (mVDAInitResult == VideoDecodeAcceleratorAdaptor::Result::SUCCESS) {
        mComponentState = ComponentState::STARTED;
        mHasError = false;
    }

    done->Signal();
}

void C2VDAComponent::onQueueWork(std::unique_ptr<C2Work> work) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onQueueWork: flags=0x%x, index=%llu, timestamp=%llu", work->input.flags,
          work->input.ordinal.frameIndex.peekull(), work->input.ordinal.timestamp.peekull());
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    uint32_t drainMode = NO_DRAIN;
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        ALOGI("input EOS");
        drainMode = DRAIN_COMPONENT_WITH_EOS;
    }

    if ((work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) == 0 && (mFirstInputTimestamp == -1)) {
        mFirstInputTimestamp = work->input.ordinal.timestamp.peekull();
    }

    CODEC2_ATRACE_INT64("c2InPTS", work->input.ordinal.timestamp.peekull());
    CODEC2_ATRACE_INT64("c2BitstreamId", work->input.ordinal.frameIndex.peekull());
    CODEC2_ATRACE_INT64("c2InPTS", 0);
    CODEC2_ATRACE_INT64("c2BitstreamId", 0);
    mInputWorkCount ++;
    if (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) {
        mInputCSDWorkCount ++;
    }
    C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s in/out[%lld(%d)-%lld(%d)]", __func__,
            mInputWorkCount, mInputCSDWorkCount,
            mOutputWorkCount, mPendingBuffersToWork.size());

    mQueue.push({std::move(work), drainMode});
    // TODO(johnylin): set a maximum size of mQueue and check if mQueue is already full.

    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onDequeueWork, ::base::Unretained(this)));
}

void C2VDAComponent::onDequeueWork() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGI("onDequeueWork Queue Size:%d", mQueue.size());
    RETURN_ON_UNINITIALIZED_OR_ERROR();
    if (mQueue.empty()) {
        return;
    }
    if (mComponentState == ComponentState::DRAINING ||
        mComponentState == ComponentState::FLUSHING) {
        ALOGV("Temporarily stop dequeueing works since component is draining/flushing.");
        return;
    }
    if (mComponentState != ComponentState::STARTED) {
        ALOGE("Work queue should be empty if the component is not in STARTED state.");
        return;
    }

    // Dequeue a work from mQueue.
    std::unique_ptr<C2Work> work(std::move(mQueue.front().mWork));
    auto drainMode = mQueue.front().mDrainMode;
    mQueue.pop();

    CHECK_LE(work->input.buffers.size(), 1u);
    bool isEmptyCSDWork = false;
    bool isEmptyWork = false;
    // Use frameIndex as bitstreamId.
    int32_t bitstreamId = frameIndexToBitstreamId(work->input.ordinal.frameIndex);
    if (work->input.buffers.empty()) {
        // Client may queue a work with no input buffer for either it's EOS or empty CSD, otherwise
        // every work must have one input buffer.
        isEmptyCSDWork = work->input.flags & C2FrameData::FLAG_CODEC_CONFIG;
        if (work->input.flags == 0) {
            isEmptyWork = true;
        }
        //CHECK(drainMode != NO_DRAIN || isEmptyCSDWork);
        // Emplace a nullptr to unify the check for work done.
        ALOGV("Got a work with no input buffer! Emplace a nullptr inside.");
        work->input.buffers.emplace_back(nullptr);
    } else if (work->input.buffers.front() != nullptr) {
        // If input.buffers is not empty, the buffer should have meaningful content inside.
        C2ConstLinearBlock linearBlock = work->input.buffers.front()->data().linearBlocks().front();
        CHECK_GT(linearBlock.size(), 0u);

        // Send input buffer to VDA for decode.
        int64_t timestamp = work->input.ordinal.timestamp.peekull();
        //check hdr10 plus
        const uint8_t *hdr10plusbuf = nullptr;
        uint32_t hdr10pluslen = 0;
        C2ReadView rView = mDefaultDummyReadView;

        for (const std::unique_ptr<C2Param> &param : work->input.configUpdate) {
            switch (param->coreIndex().coreIndex()) {
                case C2StreamHdr10PlusInfo::CORE_INDEX:
                    {
                        C2StreamHdr10PlusInfo::input *hdr10PlusInfo =
                        C2StreamHdr10PlusInfo::input::From(param.get());
                        if (hdr10PlusInfo != nullptr) {
                            std::vector<std::unique_ptr<C2SettingResult>> failures;
                            std::unique_ptr<C2Param> outParam = C2Param::CopyAsStream(*param.get(), true /* out put*/, param->stream());

                            c2_status_t err = mIntfImpl->config({outParam.get()}, C2_MAY_BLOCK, &failures);
                            if (err == C2_OK) {
                                mHDR10PlusMeteDataNeedCheck = true;
                                work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*outParam.get()));

                                rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
                                hdr10plusbuf = rView.data();
                                hdr10pluslen = rView.capacity();
                            } else {
                                ALOGE("onDequeueWork: Config update hdr10Plus size failed.");
                            }
                        }
                    }
                    break;
                default:
                    break;
            }
        }
        sendInputBufferToAccelerator(linearBlock, bitstreamId, timestamp, work->input.flags, (unsigned char *)hdr10plusbuf, hdr10pluslen);
    }

    CHECK_EQ(work->worklets.size(), 1u);
    work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(0);
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->input.ordinal.customOrdinal = work->input.ordinal.timestamp.peekull();

    if (drainMode != NO_DRAIN) {
        if (mVideoDecWraper) {
            mVideoDecWraper->eosFlush();
        }
        mComponentState = ComponentState::DRAINING;
        mPendingOutputEOS = drainMode == DRAIN_COMPONENT_WITH_EOS;
    }

    // Put work to mPendingWorks.
    ALOGI("onDequeueWork, put pendtingwokr bitid:%lld, pending worksize:%d",
            work->input.ordinal.frameIndex.peeku(), mPendingWorks.size());
    mPendingWorks.emplace_back(std::move(work));

    if (isEmptyCSDWork || isEmptyWork) {
        // Directly report the empty CSD work as finished.
        ALOGI("onDequeueWork empty csd work, bitid:%d\n", bitstreamId);
        reportWorkIfFinished(bitstreamId, 0, isEmptyWork);
    }

    if (!mQueue.empty()) {
        mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::onDequeueWork,
                                                      ::base::Unretained(this)));
    }
}

void C2VDAComponent::onInputBufferDone(int32_t bitstreamId) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onInputBufferDone: bitstream id=%d", bitstreamId);
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    C2Work* work = getPendingWorkByBitstreamId(bitstreamId);
    if (!work) {
        C2VDA_LOG(CODEC2_LOG_ERR, "[%s:%d] can not get pending work with bitstreamid:%d", __func__, __LINE__,  bitstreamId);
        reportError(C2_CORRUPTED);
        return;
    }

    // When the work is done, the input buffer shall be reset by component.
    work->input.buffers.front().reset();

    reportWorkIfFinished(bitstreamId,0);
}

void C2VDAComponent::onOutputBufferReturned(std::shared_ptr<C2GraphicBlock> block,uint32_t poolId,
                                            uint32_t blockId) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onOutputBufferReturned: block id:%u", blockId);
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    if ((block->width() != static_cast<uint32_t>(mOutputFormat.mCodedSize.width()) ||
        block->height() != static_cast<uint32_t>(mOutputFormat.mCodedSize.height())) &&
        (block->width() != mMetaDataUtil->getOutAlignedSize(mOutputFormat.mCodedSize.width(), true) ||
        block->height() != mMetaDataUtil->getOutAlignedSize(mOutputFormat.mCodedSize.height(), true))) {
        // Output buffer is returned after we changed output resolution. Just let the buffer be
        // released.
        ALOGV("Discard obsolete graphic block: pool id=%u", poolId);
        return;
    }

    GraphicBlockInfo* info = getGraphicBlockByBlockId(poolId, blockId);
    if (!info) {
        //need to rebind poolid vs blockid
        info = getUnbindGraphicBlock();
        if (!info) {
            if (!mPendingGraphicBlockBuffer) {
                mPendingGraphicBlockBuffer = std::move(block);
                mPendingGraphicBlockBufferId = blockId;
                ALOGV("now pending block id: %d, count:%ld", blockId, block.use_count());
                return;
            }
            reportError(C2_CORRUPTED);
            return;
        }
        info->mPoolId = poolId;
    }

    if (!info->mBind) {
        ALOGI("after realloc graphic, rebind %d->%d", poolId, info->mBlockId);
        info->mBind = true;
    }

    if (info->mState != GraphicBlockInfo::State::OWNED_BY_CLIENT &&
            getVideoResolutionChanged()) {
        ALOGE("Graphic block (id=%d) (state=%s) (fd=%d) (fdset=%d)should be owned by client on return",
                info->mBlockId, GraphicBlockState(info->mState), info->mFd,info->mFdHaveSet);
        displayGraphicBlockInfo();
        reportError(C2_BAD_STATE);
        return;
    }
    info->mGraphicBlock = std::move(block);
    info->mState = GraphicBlockInfo::State::OWNED_BY_COMPONENT;

    if (mPendingOutputFormat) {
        tryChangeOutputFormat();
    } else {
        // Do not pass the ownership to accelerator if this buffer will still be reused under
        // |mPendingBuffersToWork|.
        auto existingFrame = std::find_if(
                mPendingBuffersToWork.begin(), mPendingBuffersToWork.end(),
                [id = info->mBlockId](const OutputBufferInfo& o) { return o.mBlockId == id; });
        bool ownByAccelerator = existingFrame == mPendingBuffersToWork.end();
        sendOutputBufferToAccelerator(info, ownByAccelerator);
        sendOutputBufferToWorkIfAny(false /* dropIfUnavailable */);
    }
}

void C2VDAComponent::onNewBlockBufferFetched(std::shared_ptr<C2GraphicBlock> block, uint32_t poolId,
                                            uint32_t blockId) {

    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onNewBlockBufferFetched: block id:%u", blockId);
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    if (getVideoResolutionChanged()) {
        appendOutputBuffer(std::move(block), poolId, blockId, true);
        GraphicBlockInfo *info = getGraphicBlockByBlockId(poolId, blockId);
        info->mState = GraphicBlockInfo::State::OWNED_BY_COMPONENT;
        sendOutputBufferToAccelerator(info, true /*ownByAccelerator*/);
    } else {
        if ((mOutputFormat.mCodedSize.width() == block->width() &&
             mOutputFormat.mCodedSize.height() == block->height()) ||
            (mMetaDataUtil->getOutAlignedSize(mOutputFormat.mCodedSize.width(), true) == block->width() &&
             mMetaDataUtil->getOutAlignedSize(mOutputFormat.mCodedSize.height(), true) == block->height())){
            C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, "current resolution (%d*%d) new block(%d*%d) and add it",
                mOutputFormat.mCodedSize.width(), mOutputFormat.mCodedSize.height(), block->width(), block->height());
            appendOutputBuffer(std::move(block), poolId, blockId, true);
            GraphicBlockInfo *info = getGraphicBlockByBlockId(poolId, blockId);
            info->mState = GraphicBlockInfo::State::OWNED_BY_COMPONENT;
            sendOutputBufferToAccelerator(info, true /*ownByAccelerator*/);
        } else {
            C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1,"fetch current block(%d*%d) is pending", block->width(), block->height());
        }
    }
}

void C2VDAComponent::onOutputBufferDone(int32_t pictureBufferId, int64_t bitstreamId, int32_t flags) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onOutputBufferDone: picture id=%d, bitstream id=%lld, flags: %d", pictureBufferId, bitstreamId, flags);
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    int64_t timestamp = -1;
    GraphicBlockInfo* info = getGraphicBlockById(pictureBufferId);

    if (!info) {
        C2VDA_LOG(CODEC2_LOG_ERR, "[%s:%d] can not get graphicblock  with pictureBufferId:%d", __func__, __LINE__, pictureBufferId);
        reportError(C2_CORRUPTED);
        return;
    }
    if (mHDR10PlusMeteDataNeedCheck/* || (mUpdateDurationUsCount < kUpdateDurationFramesNumMax)*/) {
        unsigned char  buffer[META_DATA_SIZE];
        int buffer_size = 0;
        memset(buffer, 0, META_DATA_SIZE);
        mMetaDataUtil->getUvmMetaData(info->mFd, buffer, &buffer_size);
        if (buffer_size > META_DATA_SIZE) {
            C2VDA_LOG(CODEC2_LOG_ERR, "uvm metadata size error, please check");
        } else if (buffer_size <= 0) {
            //nothing to do now
        } else {
            mMetaDataUtil->parseAndprocessMetaData(buffer, buffer_size);
        }
        mUpdateDurationUsCount++;
    }

    if (info->mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR) {
        info->mState = GraphicBlockInfo::State::OWNED_BY_COMPONENT;
    }

    bool FoundWorkInPending = false;
    C2Work BackWork;
    C2Work* work = getPendingWorkByBitstreamId(bitstreamId);
    if (!work) {
        if (mMetaDataUtil->getUnstablePts()) {
            if (bitstreamId == mLastOutputBitstreamId && mLastOutputBitstreamId != -1) {
                work = cloneWork(&mLastOutputC2Work);
            }
        }
        if (!work) {
            C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] discard bitstreamid after flush or reset :%lld", __FUNCTION__, __LINE__, bitstreamId);
            info->mState = GraphicBlockInfo::State::OWNED_BY_COMPONENT;
            sendOutputBufferToAccelerator(info, true /* ownByAccelerator */);
            return;
        }
    } else {
        FoundWorkInPending = true;
        if (mMetaDataUtil->getUnstablePts()) {
            cloneWork(work, &BackWork);
        }
    }
    timestamp = work->input.ordinal.timestamp.peekull();
    mPendingBuffersToWork.push_back({(int32_t)bitstreamId, pictureBufferId, timestamp,flags});
    mOutputWorkCount ++;
    C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s bitstreamid=%lld, blockid(pictureid):%d, pendindbuffersize:%d",
            __func__, bitstreamId, pictureBufferId,
            mPendingBuffersToWork.size());

    CODEC2_ATRACE_INT64("c2OutPTS", timestamp);
    C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s in/out[%lld(%d)-%lld(%d)]",
            __func__,
            mInputWorkCount, mInputCSDWorkCount,
            mOutputWorkCount, mPendingBuffersToWork.size());
    CODEC2_ATRACE_INT64("c2OutPTS", 0);

    if (mMetaDataUtil->isInterlaced()) {
        //for interlaced video
        if (mOutputWorkCount == 2) {
            if (!(mInterlacedType&C2_INTERLACED_TYPE_SETUP)) {
                mInterlacedType |= C2_INTERLACED_TYPE_SETUP;
                if (bitstreamId != mLastOutputBitstreamId) {
                    mInterlacedType |= C2_INTERLACED_TYPE_1FIELD;
                } else {
                    mInterlacedType |= C2_INTERLACED_TYPE_2FIELD;
                }
                ALOGD("%s#%d setting up mInterlacedType:%08x, bitstreamId:%lld(%d)", __func__, __LINE__, mInterlacedType,bitstreamId,mLastOutputBitstreamId);
            }
        }
    }


    if (mComponentState == ComponentState::FLUSHING) {
        C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1,"%s in flushing, pending bitstreamid=%lld first", __func__, bitstreamId);
        if (mMetaDataUtil->getUnstablePts()) {
            if (!FoundWorkInPending && work) {
                delete work;
            }
        }
        mLastOutputBitstreamId = bitstreamId;
        return;
    }

    if (isNonTunnelMode()) {
        sendOutputBufferToWorkIfAny(false /* dropIfUnavailable */);
    } else if (isTunnelMode() && mTunnelHelper) {
        mTunnelHelper->sendVideoFrameToVideoTunnel(pictureBufferId, bitstreamId);
    }
    if (mMetaDataUtil->getUnstablePts()) {
        if (FoundWorkInPending) {
            cloneWork(&BackWork, &mLastOutputC2Work);
        }
        if (!FoundWorkInPending && work) {
            delete work;
        }
    }

    // The first two frames are CSD data.
    if (mOutputWorkCount == 2) {
        mDequeueLoopRunning.store(true);
        ALOGD("Enable queuethread Running...");
    }
    mLastOutputBitstreamId = bitstreamId;
}

C2Work* C2VDAComponent::cloneWork(C2Work* ori) {
    C2Work* n = new C2Work();
    if (!n) {
        ALOGE("C2VDAComponent::cloneWork, malloc memory failed.");
        return NULL;
    }
    n->input.flags = ori->input.flags;
    n->input.ordinal = ori->input.ordinal;
    n->worklets.emplace_back(new C2Worklet);
    n->worklets.front()->output.ordinal = n->input.ordinal;

    return n;
}

void C2VDAComponent::cloneWork(C2Work* ori, C2Work* out) {
    if (out == NULL || ori == NULL)
        return;

    out->input.flags = ori->input.flags;
    out->input.ordinal = ori->input.ordinal;
    out->worklets.emplace_back(new C2Worklet);
    out->worklets.front()->output.ordinal = out->input.ordinal;

    return;
}

c2_status_t C2VDAComponent::sendOutputBufferToWorkIfAny(bool dropIfUnavailable) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());

    while (!mPendingBuffersToWork.empty()) {
        auto nextBuffer = mPendingBuffersToWork.front();
        GraphicBlockInfo* info = getGraphicBlockById(nextBuffer.mBlockId);
        if (info->mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR) {
            ALOGE("Graphic block (id=%d) should not be owned by accelerator", info->mBlockId);
            reportError(C2_BAD_STATE);
            return C2_BAD_STATE;
        }
        C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL2,"%s get pendting bitstream:%d, blockid(pictueid):%d",
            __func__, nextBuffer.mBitstreamId, nextBuffer.mBlockId);
        bool sendCloneWork = false;
        C2Work* work = NULL;

        if (mMetaDataUtil->getUnstablePts()) {
            //for one packet contains more than one frame.
            if (mLastFinishedBitstreamId == -1 ||
                mLastFinishedBitstreamId != nextBuffer.mBitstreamId) {
                work = getPendingWorkByBitstreamId(nextBuffer.mBitstreamId);
            } else {
                work = cloneWork(&mLastFinishedC2Work);
                sendCloneWork = true;
            }
        } else {
            //default path
            work = getPendingWorkByBitstreamId(nextBuffer.mBitstreamId);
        }
        if (!work) {
            C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] discard bitstreamid after flush or reset :%d", __FUNCTION__, __LINE__, nextBuffer.mBitstreamId);
            info->mState = GraphicBlockInfo::State::OWNED_BY_COMPONENT;
            sendOutputBufferToAccelerator(info, true /* ownByAccelerator */);
            return C2_OK;
        }

        if (mMetaDataUtil->isInterlaced() && !mMetaDataUtil->getUnstablePts()) {
            if (!sendCloneWork) {
                if (mInterlacedType == (C2_INTERLACED_TYPE_SETUP | C2_INTERLACED_TYPE_2FIELD) &&
                    (nextBuffer.mBitstreamId != mLastFinishedBitstreamId || mLastFinishedBitstreamId == -1)) {
                    work = cloneWork(work);
                    sendCloneWork = true;
                    C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1,"%s#%d interlace one-in/two-out first field output, clone the work", __func__, __LINE__);
                } else if (!(mInterlacedType&C2_INTERLACED_TYPE_SETUP)) {
                    ALOGD("for Interlaced, first frame must be cloned.");
                    work = cloneWork(work);
                    sendCloneWork = true;
                    mNeedFinishFirstWork4Interlaced = true;
                    mOutPutInfo4WorkIncomplete = new OutputBufferInfo();
                    mOutPutInfo4WorkIncomplete->flags = nextBuffer.flags;
                    mOutPutInfo4WorkIncomplete->mBitstreamId = nextBuffer.mBitstreamId;
                    mOutPutInfo4WorkIncomplete->mBlockId = nextBuffer.mBlockId;
                    mOutPutInfo4WorkIncomplete->mMediaTimeUs = nextBuffer.mMediaTimeUs;
                }
            }
            if (mInterlacedType == (C2_INTERLACED_TYPE_SETUP | C2_INTERLACED_TYPE_1FIELD)) {
                if (mNeedFinishFirstWork4Interlaced && mOutPutInfo4WorkIncomplete) {
                    mNeedFinishFirstWork4Interlaced = false;
                    ALOGD("for Interlaced, first frame cloned.must be finished");
                    reportEmptyWork(mOutPutInfo4WorkIncomplete->mBitstreamId,mOutPutInfo4WorkIncomplete->flags);
                    delete mOutPutInfo4WorkIncomplete;
                    mOutPutInfo4WorkIncomplete = NULL;
                }
            }
        }
        mLastFinishedBitstreamId = nextBuffer.mBitstreamId;
        if (mMetaDataUtil->getUnstablePts()) {
            cloneWork(work, &mLastFinishedC2Work);
        }

        if (info->mState == GraphicBlockInfo::State::OWNED_BY_CLIENT) {
            // This buffer is the existing frame and still owned by client.
            if (!dropIfUnavailable &&
                std::find(mUndequeuedBlockIds.begin(), mUndequeuedBlockIds.end(),
                    nextBuffer.mBlockId) == mUndequeuedBlockIds.end()) {
                ALOGV("Still waiting for existing frame returned from client...");
                return C2_TIMED_OUT;
            }
            ALOGV("Drop this frame...");
            sendOutputBufferToAccelerator(info, false /* ownByAccelerator */);
            work->worklets.front()->output.flags = C2FrameData::FLAG_DROP_FRAME;

        } else if ( ((int)nextBuffer.flags & (int)PictureFlag::PICTURE_FLAG_ERROR_FRAME) != 0) {
            C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] drop error frame :%d", __FUNCTION__, __LINE__, nextBuffer.mBitstreamId);
            detectNoShowFrameWorksAndReportIfFinished(work->input.ordinal);
            sendOutputBufferToAccelerator(info, false /* ownByAccelerator */);
            work->worklets.front()->output.flags = C2FrameData::FLAG_DROP_FRAME;

        } else {
            // This buffer is ready to push into the corresponding work.
            // Output buffer will be passed to client soon along with mListener->onWorkDone_nb().
            info->mState = GraphicBlockInfo::State::OWNED_BY_CLIENT;
            mBuffersInClient++;
            updateUndequeuedBlockIds(info->mBlockId);

            // Attach output buffer to the work corresponded to bitstreamId.
            C2ConstGraphicBlock constBlock = info->mGraphicBlock->share(
                C2Rect(mOutputFormat.mVisibleRect.width(),
                mOutputFormat.mVisibleRect.height()),
                C2Fence());
            //MarkBlockPoolDataAsShared(constBlock);
            {
                //for dump
                if (mDumpYuvFp && !mSecureMode) {
                    const C2GraphicView& view = constBlock.map().get();
                    const uint8_t* const* data = view.data();
                    int size = info->mGraphicBlock->width() * info->mGraphicBlock->height() * 3 / 2;
                    //ALOGV("%s C2ConstGraphicBlock database:%x, y:%p u:%p",
                     //       __FUNCTION__, reinterpret_cast<intptr_t>(data[0]), data[C2PlanarLayout::PLANE_Y], data[C2PlanarLayout::PLANE_U]);
                    fwrite(data[0], 1, size, mDumpYuvFp);
                }
            }
#if 0
            {
                const C2Handle* chandle = constBlock.handle();
                ALOGI("sendOutputBufferToWorkIfAny count:%ld pooid:%d, fd:%d", info->mGraphicBlock.use_count(), info->mBlockId, chandle->data[0]);
            }
#endif
            std::shared_ptr<C2Buffer> buffer = C2Buffer::CreateGraphicBuffer(std::move(constBlock));
            if (mMetaDataUtil->isColorAspectsChanged()) {
                updateColorAspects();
            }
            if (mCurrentColorAspects) {
                buffer->setInfo(mCurrentColorAspects);
            }
            /* update hdr static info */
            if (mMetaDataUtil->isHDRStaticInfoUpdated()) {
                updateHDRStaticInfo();
            }
            if (mCurrentHdrStaticInfo) {
                buffer->setInfo(mCurrentHdrStaticInfo);
            }

            /* updata hdr10 plus info */
            if (mMetaDataUtil->isHDR10PlusStaticInfoUpdated()) {
                updateHDR10PlusInfo();
            }
            if (mCurrentHdr10PlusInfo) {
                buffer->setInfo(mCurrentHdr10PlusInfo);
            }

            if (mPictureSizeChanged && mCurrentSize != nullptr) {
                mPictureSizeChanged = false;
                work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*mCurrentSize));
                ALOGI("video size changed");
            }

            if (mOutputDelay != nullptr) {
                work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*mOutputDelay));
                mOutputDelay = nullptr;
            }
            work->worklets.front()->output.buffers.emplace_back(std::move(buffer));
            info->mGraphicBlock.reset();
        }

        // Check no-show frame by timestamps for VP8/VP9 cases before reporting the current work.
        if (mIntfImpl->getInputCodec() == InputCodec::VP9) {
            detectNoShowFrameWorksAndReportIfFinished(work->input.ordinal);
        }
        int64_t timestamp = work->input.ordinal.timestamp.peekull();
        ATRACE_INT("c2workpts", timestamp);
        C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL2,"sendOutputBufferToWorkIfAny bitid %d, pts:%lld", nextBuffer.mBitstreamId, timestamp);
        ATRACE_INT("c2workpts", 0);

        if (sendCloneWork) {
            sendClonedWork(work,nextBuffer.flags);
        } else {
            reportWorkIfFinished(nextBuffer.mBitstreamId,nextBuffer.flags);
        }
        mPendingBuffersToWork.pop_front();
    }
    return C2_OK;
}

void C2VDAComponent::sendClonedWork(C2Work* work, int32_t flags) {
    work->worklets.front()->output.flags = C2FrameData::FLAG_INCOMPLETE;
    work->result = C2_OK;
    work->workletsProcessed = 1;

    work->input.ordinal.customOrdinal = mMetaDataUtil->checkAndAdjustOutPts(work,flags);
    c2_cntr64_t timestamp = work->worklets.front()->output.ordinal.timestamp + work->input.ordinal.customOrdinal
                            - work->input.ordinal.timestamp;
    C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL2,"Reported finished work index=%llu pts=%llu,%d", work->input.ordinal.frameIndex.peekull(), timestamp.peekull(),__LINE__);
    std::list<std::unique_ptr<C2Work>> finishedWorks;
    finishedWorks.emplace_back(std::move(std::unique_ptr<C2Work>(work)));
    mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
}

void C2VDAComponent::updateUndequeuedBlockIds(int32_t blockId) {
    // The size of |mUndequedBlockIds| will always be the minimum buffer count for display.
    mUndequeuedBlockIds.push_back(blockId);
    mUndequeuedBlockIds.pop_front();
}

void C2VDAComponent::onDrain(uint32_t drainMode) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onDrain: mode = %u", drainMode);
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    if (!mQueue.empty()) {
        // Mark last queued work as "drain-till-here" by setting drainMode. Do not change drainMode
        // if last work already has one.
        if (mQueue.back().mDrainMode == NO_DRAIN) {
            mQueue.back().mDrainMode = drainMode;
        }
    } else if (!mPendingWorks.empty()) {
        // Neglect drain request if component is not in STARTED mode. Otherwise, enters DRAINING
        // mode and signal VDA flush immediately.
        if (mComponentState == ComponentState::STARTED) {
            if (mVideoDecWraper) {
                mVideoDecWraper->eosFlush();
            }
            mComponentState = ComponentState::DRAINING;
            mPendingOutputEOS = drainMode == DRAIN_COMPONENT_WITH_EOS;

            if (mTunnelHelper) {
                mTunnelHelper->flush();
            }
        } else {
            ALOGV("Neglect drain. Component in state: %d", mComponentState);
        }
    } else {
        // Do nothing.
        ALOGV("No buffers in VDA, drain takes no effect.");
    }
}

void C2VDAComponent::onDrainDone() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();
    if (mComponentState == ComponentState::DRAINING) {
        mComponentState = ComponentState::STARTED;
    } else if (mComponentState == ComponentState::STOPPING) {
        ALOGV(" The client signals stop right before VDA notifies drain done. Let stop process goes.");
        return;
    } else if (mComponentState != ComponentState::FLUSHING) {
        // It is reasonable to get onDrainDone in FLUSHING, which means flush is already signaled
        // and component should still expect onFlushDone callback from VDA.
        ALOGV(" Unexpected state while onDrainDone(). State=%d", mComponentState);
        reportError(C2_BAD_STATE);
        return;
    }

    if (isTunnelMode()) {
        CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d] tunnel mode reset done", __FUNCTION__, __LINE__);
        mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onDequeueWork, ::base::Unretained(this)));
        return;
    }

    // Drop all pending existing frames and return all finished works before drain done.
    if (sendOutputBufferToWorkIfAny(true /* dropIfUnavailable */) != C2_OK) {
        ALOGV(" sendOutputBufferToWorkIfAny failed.") ;
        return;
    }

    if (mPendingOutputEOS) {
        ALOGI(" Return EOS work.");
        if (reportEOSWork() != C2_OK) {
            return;
        }
    }

    // Work dequeueing was stopped while component draining. Restart it.
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onDequeueWork, ::base::Unretained(this)));
}

void C2VDAComponent::onFlush() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onFlush");
    if (mComponentState == ComponentState::FLUSHING ||
        mComponentState == ComponentState::STOPPING) {
        return;  // Ignore other flush request when component is flushing or stopping.
    }
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    if (mVideoDecWraper) {
        mVideoDecWraper->flush();
    }

    if (mTunnelHelper) {
        mTunnelHelper->flush();
    }

    // Pop all works in mQueue and put into mAbandonedWorks.
    while (!mQueue.empty()) {
        mAbandonedWorks.emplace_back(std::move(mQueue.front().mWork));
        mQueue.pop();
    }
    mComponentState = ComponentState::FLUSHING;
    mLastFlushTimeMs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000000;
    mFirstInputTimestamp = -1;
    mLastOutputBitstreamId = -1;
    mLastFinishedBitstreamId = -1;
    if (mOutPutInfo4WorkIncomplete) {
        delete mOutPutInfo4WorkIncomplete;
        mOutPutInfo4WorkIncomplete = NULL;
    }
}

void C2VDAComponent::onStop(::base::WaitableEvent* done) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGI(" EOS onStop");
    if (mComponentState == ComponentState::UNINITIALIZED) {
        return;
    }
    // Stop call should be processed even if component is in error state.

    // Pop all works in mQueue and put into mAbandonedWorks.
    while (!mQueue.empty()) {
        mAbandonedWorks.emplace_back(std::move(mQueue.front().mWork));
        mQueue.pop();
    }

    mStopDoneEvent = done;  // restore done event which shoud be signaled in onStopDone().
    mComponentState = ComponentState::STOPPING;

    // Immediately release VDA by calling onStopDone() if component is in error state. Otherwise,
    // send reset request to VDA and wait for callback to stop the component gracefully.
    if (mHasError) {
        ALOGV("Component is in error state. Immediately call onStopDone().");
        onStopDone();
    } else if (mComponentState != ComponentState::FLUSHING) {
        // Do not request VDA reset again before the previous one is done. If reset is already sent
        // by onFlush(), just regard the following NotifyResetDone callback as for stopping.
        uint32_t flags = 0;
        if (mVideoDecWraper) {
            mVideoDecWraper->stop(flags|RESET_FLAG_NOWAIT);
        }
    }
}

void C2VDAComponent::resetInputAndOutputBufInfo() {
    mInputWorkCount = 0;
    mInputCSDWorkCount = 0;
    mOutputWorkCount = 0;
    mHasQueuedWork = false;
    mUpdateDurationUsCount = 0;
}

void C2VDAComponent::onFlushOrStopDone() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    if (mComponentState == ComponentState::UNINITIALIZED) {
        return;  // component is already stopped.
    }
    if (mComponentState == ComponentState::FLUSHING) {
        mLastFlushTimeMs = 0;
        onFlushDone();
        resetInputAndOutputBufInfo();
    } else if (mComponentState == ComponentState::STOPPING) {
        onStopDone();
        resetInputAndOutputBufInfo();
    } else {
        ALOGE("%s:%d", __FUNCTION__, __LINE__);
        reportError(C2_CORRUPTED);
    }
}

void C2VDAComponent::onFlushDone() {
    ALOGV("onFlushDone");
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    reportAbandonedWorks();
    while (!mPendingBuffersToWork.empty()) {
        auto nextBuffer = mPendingBuffersToWork.front();
        GraphicBlockInfo* info = getGraphicBlockById(nextBuffer.mBlockId);
        if (info->mState == GraphicBlockInfo::State::OWNED_BY_COMPONENT) {
            info->mState = GraphicBlockInfo::State::OWNED_BY_ACCELERATOR;
        }
        mPendingBuffersToWork.pop_front();
    }
    mPendingBuffersToWork.clear();
    mComponentState = ComponentState::STARTED;


    //after flush we need reuse the buffer which owned by accelerator
    for (auto& info : mGraphicBlocks) {
        ALOGV("%s index:%d,graphic block status:%s count:%ld", __func__,
                info.mBlockId, GraphicBlockState(info.mState), info.mGraphicBlock.use_count());
        if (info.mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR) {
            ALOGE("sendOutputBufferToAccelerator ");
            sendOutputBufferToAccelerator(&info, false);
        }
    }
    mMetaDataUtil->flush();

    // Work dequeueing was stopped while component flushing. Restart it.
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onDequeueWork, ::base::Unretained(this)));
}

void C2VDAComponent::onStopDone() {
    ALOGV("onStopDone");
    CHECK(mStopDoneEvent);

    // TODO(johnylin): At this moment, there may be C2Buffer still owned by client, do we need to
    // do something for them?
    reportAbandonedWorks();
    mPendingOutputFormat.reset();
    mPendingBuffersToWork.clear();
    stopDequeueThread();
    mHasQueuedWork = false;

    if (mVideoDecWraper) {
        mVideoDecWraper->destroy();
        mVideoDecWraper.reset();
        mVideoDecWraper = NULL;
        if (mMetaDataUtil) {
            mMetaDataUtil.reset();
            mMetaDataUtil = NULL;
        }
    }
    if (mTunnelHelper) {
        mTunnelHelper->stop();
        mTunnelHelper.reset();
        mTunnelHelper = NULL;
    }

    if (mPendingGraphicBlockBuffer) {
        ALOGV("clear pending block id: %d, count:%ld",
                mPendingGraphicBlockBufferId, mPendingGraphicBlockBuffer.use_count());
        mPendingGraphicBlockBufferId = -1;
        mPendingGraphicBlockBuffer.reset();
    }

    mBufferFirstAllocated = false;
    mSurfaceUsageGeted = false;
    mStopDoneEvent->Signal();
    for (auto& info : mGraphicBlocks) {
        ALOGV("GraphicBlock reset, block Info Id:%d Fd:%d poolId:%d State:%s block use count:%ld",
            info.mBlockId, info.mFd, info.mPoolId, GraphicBlockState(info.mState), info.mGraphicBlock.use_count());
            info.mGraphicBlock.reset();
    }
    ALOGV("clear GraphicBlocks");
    mGraphicBlocks.clear();
    if (mBlockPoolUtil != NULL) {
        mBlockPoolUtil->cancelAllGraphicBlock();
        mBlockPoolUtil.reset();
        mBlockPoolUtil = NULL;
    }

    mStopDoneEvent = nullptr;
    mComponentState = ComponentState::UNINITIALIZED;
    ALOGV("onStopDone OK");
}

c2_status_t C2VDAComponent::setListener_vb(const std::shared_ptr<C2Component::Listener>& listener,
        c2_blocking_t mayBlock) {
    UNUSED(mayBlock);
    // TODO(johnylin): API says this method must be supported in all states, however I'm quite not
    //                 sure what is the use case.
    if (mState.load() != State::LOADED) {
        return C2_BAD_STATE;
    }
    mListener = listener;
    return C2_OK;
}

void C2VDAComponent::sendInputBufferToAccelerator(const C2ConstLinearBlock& input,
        int32_t bitstreamId, uint64_t timestamp,int32_t flags,uint8_t *hdrbuf,uint32_t hdrlen) {
    //UNUSED(flags);
    ALOGV("sendInputBufferToAccelerator");
    int dupFd = dup(input.handle()->data[0]);
    if (dupFd < 0) {
        ALOGE("Failed to dup(%d) input buffer (bitstreamId=%d), errno=%d", input.handle()->data[0],
                bitstreamId, errno);
        reportError(C2_CORRUPTED);
        return;
    }
    ALOGV("[%s@%d]Decode bitstream ID: %d timstamp:%llu offset: %u size: %u hdrlen:%d flags 0x%x", __FUNCTION__,__LINE__,
            bitstreamId, timestamp, input.offset(), input.size(), hdrlen,flags);
    if (mVideoDecWraper) {
        mMetaDataUtil->save_stream_info(timestamp,input.size());
        mVideoDecWraper->decode(bitstreamId, dupFd, input.offset(), input.size(), timestamp, hdrbuf, hdrlen, flags);
    }
}

std::deque<std::unique_ptr<C2Work>>::iterator C2VDAComponent::findPendingWorkByBitstreamId(
        int32_t bitstreamId) {
    return std::find_if(mPendingWorks.begin(), mPendingWorks.end(),
            [bitstreamId](const std::unique_ptr<C2Work>& w) {
            return frameIndexToBitstreamId(w->input.ordinal.frameIndex) ==
            bitstreamId;
            });
}

std::deque<std::unique_ptr<C2Work>>::iterator C2VDAComponent::findPendingWorkByMediaTime(
        int64_t mediaTime) {
    return std::find_if(mPendingWorks.begin(), mPendingWorks.end(),
            [mediaTime](const std::unique_ptr<C2Work>& w) {
            return w->input.ordinal.timestamp.peekull() ==
            mediaTime;
            });
}

C2Work* C2VDAComponent::getPendingWorkByBitstreamId(int32_t bitstreamId) {
    auto workIter = findPendingWorkByBitstreamId(bitstreamId);
    if (workIter == mPendingWorks.end()) {
        C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, [%s] "Can't find pending work by bitstream ID: %d", __func__, bitstreamId);
        return nullptr;
    }
    return workIter->get();
}

C2Work* C2VDAComponent::getPendingWorkByMediaTime(int64_t mediaTime) {
    auto workIter = findPendingWorkByMediaTime(mediaTime);
    if (workIter == mPendingWorks.end()) {
        C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s] Can't find pending work by mediaTime: %lld", __func__, mediaTime);
        return nullptr;
    }
    return workIter->get();
}

C2VDAComponent::GraphicBlockInfo* C2VDAComponent::getGraphicBlockById(int32_t blockId) {
    auto blockIter = std::find_if(mGraphicBlocks.begin(), mGraphicBlocks.end(),
        [blockId](const GraphicBlockInfo& gb) {
            return gb.mBlockId == blockId;
    });

    if (blockIter == mGraphicBlocks.end()) {
        C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s]get GraphicBlock failed: blockId=%d", __func__, blockId);
        return nullptr;
    }

    return &(*blockIter);
}

C2VDAComponent::GraphicBlockInfo* C2VDAComponent::getGraphicBlockByBlockId(uint32_t poolId,uint32_t blockId) {
    auto blockIter = std::find_if(mGraphicBlocks.begin(), mGraphicBlocks.end(),
            [poolId,blockId](const GraphicBlockInfo& gb) {
                if (gb.mPoolId == poolId) {
                    return gb.mBlockId == blockId;
                }
                return false;
        });

    if (blockIter == mGraphicBlocks.end()) {
        C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s]get GraphicBlock failed: poolId=%u", __func__, poolId);
        return nullptr;
    }
    return &(*blockIter);
}

C2VDAComponent::GraphicBlockInfo* C2VDAComponent::getGraphicBlockByFd(int32_t fd) {
    auto blockIter = std::find_if(mGraphicBlocks.begin(), mGraphicBlocks.end(),
            [fd](const GraphicBlockInfo& gb) {
            return gb.mFd == fd;
            });

    if (blockIter == mGraphicBlocks.end()) {
        C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s] get GraphicBlock failed: fd=%u", __func__, fd);
        return nullptr;
    }
    return &(*blockIter);
}

std::deque<C2VDAComponent::OutputBufferInfo>::iterator C2VDAComponent::findPendingBuffersToWorkByTime(int64_t timeus) {
    return std::find_if(mPendingBuffersToWork.begin(), mPendingBuffersToWork.end(),
            [time=timeus](const OutputBufferInfo& o) {
                return o.mMediaTimeUs == time;});
}

bool C2VDAComponent::erasePendingBuffersToWorkByTime(int64_t timeus) {
   auto buffer = findPendingBuffersToWorkByTime(timeus);
   if (buffer != mPendingBuffersToWork.end()) {
       mPendingBuffersToWork.erase(buffer);
   }

   return C2_OK;
}


C2VDAComponent::GraphicBlockInfo* C2VDAComponent::getUnbindGraphicBlock() {
    auto blockIter = std::find_if(mGraphicBlocks.begin(), mGraphicBlocks.end(),
            [&](const GraphicBlockInfo& gb) {
            return gb.mBind == false;
            });
    if (blockIter == mGraphicBlocks.end()) {
        ALOGE("getUnbindGraphicBlock failed\n");
        return nullptr;
    }
    return &(*blockIter);
}

void C2VDAComponent::onOutputFormatChanged(std::unique_ptr<VideoFormat> format) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    C2VDA_LOG(CODEC2_LOG_INFO, "[%s:%d]New output format(pixel_format=0x%x, min_num_buffers=%u, coded_size=%s, crop_rect=%s)",
            __func__, __LINE__,
            static_cast<uint32_t>(format->mPixelFormat), format->mMinNumBuffers,
            format->mCodedSize.ToString().c_str(), format->mVisibleRect.ToString().c_str());

    mCanQueueOutBuffer = false;
    for (auto& info : mGraphicBlocks) {
        C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] index:%d,graphic block status:%s count:%ld",
                __func__, __LINE__,
                info.mBlockId, GraphicBlockState(info.mState), info.mGraphicBlock.use_count());
        if (info.mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR)
            info.mState = GraphicBlockInfo::State::OWNED_BY_COMPONENT;
    }

    CHECK(!mPendingOutputFormat);
    mPendingOutputFormat = std::move(format);
    tryChangeOutputFormat();
}

void C2VDAComponent::tryChangeOutputFormat() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("tryChangeOutputFormat");
    CHECK(mPendingOutputFormat);

    // At this point, all output buffers should not be owned by accelerator. The component is not
    // able to know when a client will release all owned output buffers by now. But it is ok to
    // leave them to client since componenet won't own those buffers anymore.
    // TODO(johnylin): we may also set a parameter for component to keep dequeueing buffers and
    //                 change format only after the component owns most buffers. This may prevent
    //                 too many buffers are still on client's hand while component starts to
    //                 allocate more buffers. However, it leads latency on output format change.
    for (const auto& info : mGraphicBlocks) {
        if (info.mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR) {
            ALOGE("Graphic block (id=%d) should not be owned by accelerator while changing format",
                    info.mBlockId);
            reportError(C2_BAD_STATE);
            return;
        }
    }

    // Drop all pending existing frames and return all finished works before changing output format.
    if (isNonTunnelMode() && sendOutputBufferToWorkIfAny(true /* dropIfUnavailable */) != C2_OK) {
        return;
    }

    if (mBufferFirstAllocated) {
        mResChStat = C2_RESOLUTION_CHANGEING;
    }

    CHECK_EQ(mPendingOutputFormat->mPixelFormat, HalPixelFormat::YCRCB_420_SP);

    mLastOutputFormat.mPixelFormat = mOutputFormat.mPixelFormat;
    mLastOutputFormat.mMinNumBuffers =  mOutputFormat.mMinNumBuffers;
    mLastOutputFormat.mCodedSize = mOutputFormat.mCodedSize;

    mOutputFormat.mPixelFormat = mPendingOutputFormat->mPixelFormat;
    mOutputFormat.mMinNumBuffers = mPendingOutputFormat->mMinNumBuffers;
    mOutputFormat.mCodedSize = mPendingOutputFormat->mCodedSize;

    setOutputFormatCrop(mPendingOutputFormat->mVisibleRect);

    if (isNonTunnelMode()) {
        uint32_t dequeueBufferNum = mOutputFormat.mMinNumBuffers - kDefaultSmoothnessFactor;
        if (!mUseBufferQueue) {
            dequeueBufferNum = mOutputFormat.mMinNumBuffers;
        }
        ALOGI("update deque buffer count:%d", dequeueBufferNum);
        C2PortActualDelayTuning::output outputDelay(dequeueBufferNum);
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        c2_status_t outputDelayErr = mIntfImpl->config({&outputDelay}, C2_MAY_BLOCK, &failures);
        if (outputDelayErr == OK) {
            mOutputDelay = std::make_shared<C2PortActualDelayTuning::output>(std::move(outputDelay));
        }
    }

    if (mBufferFirstAllocated) {
        //resolution change
        if (isNonTunnelMode()) {
            videoResolutionChange();
        } else if (isTunnelMode() && mTunnelHelper) {
            mTunnelHelper->videoResolutionChangeTunnel();
        }
        return;
    }

    c2_status_t err =  allocateBuffersFromBlockPool(
            mPendingOutputFormat->mCodedSize,
            static_cast<uint32_t>(mPendingOutputFormat->mPixelFormat));

    if (err != C2_OK) {
        reportError(err);
        return;
    }

    mPendingOutputFormat.reset();
    mBufferFirstAllocated = true;
}

c2_status_t C2VDAComponent::videoResolutionChange() {
    ALOGV("videoResolutionChange");

    mPendingOutputFormat.reset();
    stopDequeueThread();

    if (mBlockPoolUtil->isBufferQueue()) {
        if (mMetaDataUtil->getNeedReallocBuffer()) {
            for (auto& info : mGraphicBlocks) {
                info.mFdHaveSet = false;
                info.mBind = false;
                info.mGraphicBlock.reset();
                mBlockPoolUtil->resetGraphicBlock(info.mBlockId);
                ALOGI("change reset block id:%d, count:%ld", info.mBlockId, info.mGraphicBlock.use_count());
                if (mPendingGraphicBlockBuffer) {
                    ALOGV("change reset pending block id: %d, count:%ld",
                        mPendingGraphicBlockBufferId, mPendingGraphicBlockBuffer.use_count());
                    mBlockPoolUtil->resetGraphicBlock(mPendingGraphicBlockBufferId);
                    mPendingGraphicBlockBufferId = -1;
                    mPendingGraphicBlockBuffer.reset();
                }
            }
            size_t inc_buf_num = mOutputFormat.mMinNumBuffers - mLastOutputFormat.mMinNumBuffers;
            size_t bufferCount = mOutputFormat.mMinNumBuffers + kDpbOutputBufferExtraCount;
            ALOGV("increase buffer num:%d graphic blocks size: %d", inc_buf_num, mGraphicBlocks.size());
            auto err = mBlockPoolUtil->requestNewBufferSet(static_cast<int32_t>(bufferCount));
            if (err != C2_OK) {
                ALOGE("failed to request new buffer set to block pool: %d", err);
                reportError(err);
            }
            if (mVideoDecWraper) {
                mVideoDecWraper->assignPictureBuffers(mOutputFormat.mMinNumBuffers);
            }
        } else {
            ALOGV("%s:%d do not need realloc", __func__, __LINE__);
            if (mVideoDecWraper) {
                mVideoDecWraper->assignPictureBuffers(mOutputFormat.mMinNumBuffers);
            }
            for (auto& info : mGraphicBlocks) {
                info.mFdHaveSet = false;
                if (info.mState == GraphicBlockInfo::State::OWNED_BY_COMPONENT) {
                    sendOutputBufferToAccelerator(&info, true /* ownByAccelerator */);
                }
            }
        }
        if (!startDequeueThread(mOutputFormat.mCodedSize, static_cast<uint32_t>(mOutputFormat.mPixelFormat),
                    false /* resetBuffersInClient */)) {
            ALOGE("%s:%d startDequeueThread failed", __func__, __LINE__);
            reportError(C2_CORRUPTED);
            return C2_CORRUPTED;
        }
    } else {
        bool reallocOutputBuffer = mMetaDataUtil->checkReallocOutputBuffer(mLastOutputFormat, mOutputFormat);
        ALOGV("realloc output buffer:%d", reallocOutputBuffer);
        if (reallocOutputBuffer) {
             for (auto& info : mGraphicBlocks) {
                ALOGV("info state: BlockId(%d) Fd(%d) State(%s) FdHaveSet(%d) use_count(%ld)",
                info.mBlockId, info.mFd, GraphicBlockState(info.mState), info.mFdHaveSet, info.mGraphicBlock.use_count());
            }

            for (auto& info : mGraphicBlocks) {
                mBlockPoolUtil->resetGraphicBlock(info.mBlockId);
                info.mFdHaveSet = false;
                info.mBind = false;
                info.mBlockId = -1;
                info.mGraphicBlock.reset();
            }
            /* clear all block */
            mGraphicBlocks.clear();
            mBlockPoolUtil->cancelAllGraphicBlock();
            auto err = allocateBuffersFromBlockPool(
                                mOutputFormat.mCodedSize,
                                static_cast<uint32_t>(mOutputFormat.mPixelFormat));
            if (err != C2_OK) {
                ALOGE("failed to allocate new buffer err: %d", err);
                reportError(err);
            }
        } else {
            for (auto& info : mGraphicBlocks) {
                info.mFdHaveSet = false;
                if (info.mState == GraphicBlockInfo::State::OWNED_BY_COMPONENT) {
                    mBlockPoolUtil->resetGraphicBlock(info.mBlockId);
                }
            }

            if (mVideoDecWraper) {
                mVideoDecWraper->assignPictureBuffers(mOutputFormat.mMinNumBuffers);
            }

            if (!startDequeueThread(mOutputFormat.mCodedSize, static_cast<uint32_t>(mOutputFormat.mPixelFormat), false)) {
                ALOGE("%s:%d startDequeueThread failed", __func__, __LINE__);
                reportError(C2_CORRUPTED);
                return C2_CORRUPTED;
            }
        }
    }

    //update picture size
    C2StreamPictureSizeInfo::output videoSize(0u, mOutputFormat.mCodedSize.width(), mOutputFormat.mCodedSize.height());
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    c2_status_t err = mIntfImpl->config({&videoSize}, C2_MAY_BLOCK, &failures);
    mPictureSizeChanged = true;
    mCurrentSize = std::make_shared<C2StreamPictureSizeInfo::output>(0u, mOutputFormat.mCodedSize.width(),
            mOutputFormat.mCodedSize.height());
    if (err != OK) {
       ALOGV("video size changed, update to params fail");
    }

    return C2_OK;
}

int C2VDAComponent::getDefaultMaxBufNum(InputCodec videotype) {
    int defaultMaxBuffers = 16;

    if (videotype == InputCodec::AV1) {
        defaultMaxBuffers = 16;
    } else if (videotype == InputCodec::VP9) {
        defaultMaxBuffers = 14;
    } else if (videotype == InputCodec::H265) {
        defaultMaxBuffers = 12;
    } else if (videotype == InputCodec::H264) {
        defaultMaxBuffers = 12;
    }

    return defaultMaxBuffers;
}

bool C2VDAComponent::getVideoResolutionChanged() {
    if (mResChStat == C2_RESOLUTION_CHANGEING) {
        for (auto& info : mGraphicBlocks) {
            if (info.mFdHaveSet == false)
                return false;
        }
        mResChStat = C2_RESOLUTION_CHANGED;
        ALOGI("video resolution changed Successfully");
    }

    return true;
}

c2_status_t C2VDAComponent::reallocateBuffersForUsageChanged(const media::Size& size,
                                                              uint32_t pixelFormat) {
    ALOGV("reallocateBuffers(%s, 0x%x)", size.ToString().c_str(), pixelFormat);

    // Get block pool ID configured from the client.
    std::shared_ptr<C2BlockPool> blockPool;
    int64_t poolId = -1;
    c2_status_t err;
    if (!mBlockPoolUtil) {
        poolId = mIntfImpl->getBlockPoolId();
        err = GetCodec2BlockPool(poolId, shared_from_this(), &blockPool);
        if (err != C2_OK) {
            ALOGE("Graphic block allocator is invalid");
            reportError(err);
            return err;
        }
    }

    bool useBufferQueue = blockPool->getAllocatorId() == C2PlatformAllocatorStore::BUFFERQUEUE;
    if (!useBufferQueue) {
        ALOGE("Graphic block allocator is invalid");
        reportError(C2_CORRUPTED);
        return C2_CORRUPTED;
    }

    size_t bufferCount = mOutputFormat.mMinNumBuffers + kDpbOutputBufferExtraCount;
    mGraphicBlocks.clear();

    if (useBufferQueue) {
        mUseBufferQueue = true;
        ALOGI("Using C2BlockPool ID:%" PRIu64 " for allocating output buffers, blockpooolid:%d",
                poolId, blockPool->getAllocatorId());
    }

    size_t minBuffersForDisplay = 0;
    // Set requested buffer count to C2BlockPool.
    err = mBlockPoolUtil->requestNewBufferSet(static_cast<int32_t>(bufferCount));
    if (err != C2_OK) {
        ALOGE("failed to request new buffer set to block pool: %d", err);
        reportError(err);
        return err;
    }
    err = mBlockPoolUtil->getMinBuffersForDisplay(&minBuffersForDisplay);
    if (err != C2_OK) {
        ALOGE("failed to query minimum undequeued buffer count from block pool: %d", err);
        reportError(err);
        return err;
    }
    int64_t surfaceUsage = mBlockPoolUtil->getConsumerUsage();
    if (!(surfaceUsage & GRALLOC_USAGE_HW_COMPOSER)) {
        mMetaDataUtil->setUseSurfaceTexture(true);
    } else {
        mMetaDataUtil->setUseSurfaceTexture(false);
        mMetaDataUtil->setForceFullUsage(true);
    }

    ALOGV("Minimum undequeued buffer count = %zu", minBuffersForDisplay);
    mUndequeuedBlockIds.resize(minBuffersForDisplay, -1);

    uint64_t platformUsage = mMetaDataUtil->getPlatformUsage();
    C2MemoryUsage usage = {
            mSecureMode ? (C2MemoryUsage::READ_PROTECTED | C2MemoryUsage::WRITE_PROTECTED) :
            (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE), platformUsage};

    ALOGV("usage %llx", usage.expected);

    for (size_t i = 0; i < bufferCount; ++i) {
        std::shared_ptr<C2GraphicBlock> block;

        int32_t retries_left = kAllocateBufferMaxRetries;
        err = C2_NO_INIT;
        while (err != C2_OK) {
            ALOGI("fetchGraphicBlock IN ALLOCATOR\n");
            err = mBlockPoolUtil->fetchGraphicBlock(mMetaDataUtil->getOutAlignedSize(size.width()),
                                               mMetaDataUtil->getOutAlignedSize(size.height()),
                                               pixelFormat, usage, &block);
            if (err == C2_TIMED_OUT && retries_left > 0) {
                ALOGD("allocate buffer timeout, %d retry time(s) left...", retries_left);
                retries_left--;
            } else if (err != C2_OK) {
                mGraphicBlocks.clear();
                ALOGE("failed to allocate buffer: %d", err);
                reportError(err);
                return err;
            }
        }

        uint32_t blockId;
        C2BlockPool::local_id_t poolId;
        mBlockPoolUtil->getPoolId(&poolId);
        err = mBlockPoolUtil->getBlockIdByGraphicBlock(block, &blockId);
        if (err != C2_OK) {
            mGraphicBlocks.clear();
            ALOGE("failed to getBlockIdByGraphicBlock: %d", err);
            reportError(err);
            return err;
        }

        if (i == 0) {
            // Allocate the output buffers.
            if (mVideoDecWraper) {
                mVideoDecWraper->assignPictureBuffers(bufferCount);
            }
            mCanQueueOutBuffer = true;
        }
        appendOutputBuffer(std::move(block), poolId, blockId,true);
        GraphicBlockInfo *info = getGraphicBlockByBlockId(poolId, blockId);
        sendOutputBufferToAccelerator(info, true /*ownByAccelerator*/);
    }

    mOutputFormat.mMinNumBuffers = bufferCount;
    if (isNonTunnelMode() && !startDequeueThread(size, pixelFormat,
                            true /* resetBuffersInClient */)) {

        ALOGE("%s:%d startDequeueThread failed", __func__, __LINE__);
        reportError(C2_CORRUPTED);
        return C2_CORRUPTED;
    }
    return C2_OK;
}

c2_status_t C2VDAComponent::allocNonTunnelBuffers(const media::Size& size, uint32_t pixelFormat) {
    size_t bufferCount = mOutBufferCount;
    int64_t surfaceUsage = 0;
    size_t minBuffersForDisplay = 0;
    C2BlockPool::local_id_t poolId = -1;

    mBlockPoolUtil->requestNewBufferSet(bufferCount - kDefaultSmoothnessFactor);
    c2_status_t err = mBlockPoolUtil->getMinBuffersForDisplay(&minBuffersForDisplay);
    if (err != C2_OK) {
        ALOGE("Graphic block allocator is invalid");
        reportError(err);
        return err;
    }
    ALOGV("Minimum undequeued buffer count:%zu buffer count:%d", minBuffersForDisplay, bufferCount);
    mUndequeuedBlockIds.resize(minBuffersForDisplay, -1);
    uint64_t platformUsage = mMetaDataUtil->getPlatformUsage();
    C2MemoryUsage usage = {
            mSecureMode ? (C2MemoryUsage::READ_PROTECTED | C2MemoryUsage::WRITE_PROTECTED) :
            (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE), platformUsage};

    ALOGV("usage %llx", usage.expected);
    // The number of buffers requested for the first time is the number defined in the framework.
    int32_t dequeue_buffer_num = 2 + kDefaultSmoothnessFactor;
    if (!mUseBufferQueue) {
        dequeue_buffer_num = bufferCount;
    }
    ALOGV("Minimum undequeued buffer count:%zu buffer count:%d first_bufferNum:%d", minBuffersForDisplay, bufferCount, dequeue_buffer_num);
    for (size_t i = 0; i < dequeue_buffer_num; ++i) {
        std::shared_ptr<C2GraphicBlock> block;

        int32_t retries_left = kAllocateBufferMaxRetries;
        err = C2_NO_INIT;
        while (err != C2_OK) {
            ALOGI("fetchGraphicBlock IN ALLOCATOR\n");
            err = mBlockPoolUtil->fetchGraphicBlock(mMetaDataUtil->getOutAlignedSize(size.width()),
                                            mMetaDataUtil->getOutAlignedSize(size.height()),
                                            pixelFormat, usage, &block);
            if (err == C2_TIMED_OUT && retries_left > 0) {
                ALOGD("allocate buffer timeout, %d retry time(s) left...", retries_left);
                if (retries_left == kAllocateBufferMaxRetries && mUseBufferQueue) {
                    int64_t newSurfaceUsage = mBlockPoolUtil->getConsumerUsage();
                    if (newSurfaceUsage != surfaceUsage) {
                        return reallocateBuffersForUsageChanged(size, pixelFormat);
                    }
                }
                retries_left--;
            } else if (err == EAGAIN) {
                ALOGE("failed to allocate buffer: %d retry i = %d", err, i);
                ::usleep(kDequeueRetryDelayUs);
            } else if (err != C2_OK) {
                //mGraphicBlocks.clear();
                ALOGE("%s@%dfailed to allocate buffer: %d", __func__, __LINE__, err);
                ::usleep(kDequeueRetryDelayUs);
                //reportError(err);
                //return err;
                //break;
            }
        }

        poolId = -1;
        uint32_t blockId = 0;
        mBlockPoolUtil->getPoolId(&poolId);
        err = mBlockPoolUtil->getBlockIdByGraphicBlock(block, &blockId);

        if (err != C2_OK) {
            mGraphicBlocks.clear();
            ALOGE("failed to getPoolIdFromGraphicBlock: %d", err);
            reportError(err);
            return err;
        }

        if (i == 0) {
            // Allocate the output buffers.
            if (mVideoDecWraper) {
                mVideoDecWraper->assignPictureBuffers(bufferCount);
            }
            mCanQueueOutBuffer = true;
        }
        appendOutputBuffer(std::move(block), poolId, blockId, true);
        GraphicBlockInfo *info = getGraphicBlockByBlockId(poolId, blockId);
        sendOutputBufferToAccelerator(info, true /*ownByAccelerator*/);
    }

    mOutputFormat.mMinNumBuffers = bufferCount;
    if (!startDequeueThread(size, pixelFormat,
                            true /* resetBuffersInClient */)) {

        ALOGE("%s:%d startDequeueThread failed", __func__, __LINE__);
        reportError(C2_CORRUPTED);
        return C2_CORRUPTED;
    }

    return C2_OK;
}

c2_status_t C2VDAComponent::allocateBuffersFromBlockPool(const media::Size& size,
                                                              uint32_t pixelFormat) {
    ALOGI("allocateBuffersFromBlockPool(%s, 0x%x)", size.ToString().c_str(), pixelFormat);
    stopDequeueThread();
    size_t bufferCount = mOutputFormat.mMinNumBuffers + kDpbOutputBufferExtraCount;
    if (isTunnelMode() || !mMetaDataUtil->getNeedReallocBuffer()) {
        mOutBufferCount = getDefaultMaxBufNum(GetIntfImpl()->getInputCodec());
        if (bufferCount > mOutBufferCount) {
            C2VDA_LOG(CODEC2_LOG_INFO, "required outbuffer count %d large than default num %d", bufferCount, mOutBufferCount);
            mOutBufferCount = bufferCount;
        } else {
            bufferCount = mOutBufferCount;
        }
    }
    mOutBufferCount = bufferCount;
    mGraphicBlocks.clear();

    // Get block pool ID configured from the client.
    std::shared_ptr<C2BlockPool> blockPool;
    C2BlockPool::local_id_t poolId = -1;
    c2_status_t err;
    if (mBlockPoolUtil == nullptr) {
        poolId = mIntfImpl->getBlockPoolId();
        err = GetCodec2BlockPool(poolId, shared_from_this(), &blockPool);
        if (err != C2_OK) {
            ALOGE("Graphic block allocator is invalid");
            reportError(err);
            return err;
        }
        ALOGI("Using C2BlockPool ID:%" PRIu64 " for allocating output buffers, allocator id:%d", poolId, blockPool->getAllocatorId());
        mBlockPoolUtil = std::make_shared<C2VDABlockPoolUtil> (blockPool);
    }

    int64_t surfaceUsage = 0;
    bool usersurfacetexture = false;
    if (mBlockPoolUtil->isBufferQueue()) {
        mUseBufferQueue = true;
        surfaceUsage = mBlockPoolUtil->getConsumerUsage();
        ALOGV("get block pool usage:%lld", surfaceUsage);
        if (!(surfaceUsage & GRALLOC_USAGE_HW_COMPOSER)) {
            usersurfacetexture = true;
            mMetaDataUtil->setUseSurfaceTexture(true);
        }
    } else {
        if (isNonTunnelMode()) {
            mMetaDataUtil->setNoSurface(true);
        }
    }

    if (isNonTunnelMode()) {
        allocNonTunnelBuffers(size, pixelFormat);
    } else if (isTunnelMode() && mTunnelHelper){
        mTunnelHelper->allocTunnelBuffersAndSendToDecoder(size, pixelFormat);
    }

    return C2_OK;
}

void C2VDAComponent::appendOutputBuffer(std::shared_ptr<C2GraphicBlock> block, uint32_t poolId,uint32_t blockId, bool bind) {
    GraphicBlockInfo info;
    int fd = 0;
    mBlockPoolUtil->getBlockFd(block, &fd);
    if (bind) {
        info.mPoolId  = poolId;
        info.mBlockId = blockId;
        info.mGraphicBlock = std::move(block);
        info.mFd = fd;
        ALOGV("%s graphicblock: %p,fd:%d blockid: %d, size: %dx%d bind %d->%d", __func__, info.mGraphicBlock->handle(), fd,
            info.mBlockId, info.mGraphicBlock->width(), info.mGraphicBlock->height(), info.mPoolId, info.mBlockId);
    }
    info.mBind = bind;
    info.mFdHaveSet = false;
    mGraphicBlocks.push_back(std::move(info));
    ALOGI("%s %d GraphicBlock Size:%d", __func__, __LINE__, mGraphicBlocks.size());
}



void C2VDAComponent::sendOutputBufferToAccelerator(GraphicBlockInfo* info, bool ownByAccelerator) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("sendOutputBufferToAccelerator mBlockId=%d ownByAccelerator=%d, poolid:%d blockid:%d", info->mBlockId,
          ownByAccelerator, info->mPoolId, info->mBlockId);

    if (ownByAccelerator) {
        CHECK_EQ(info->mState, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
        info->mState = GraphicBlockInfo::State::OWNED_BY_ACCELERATOR;
    }

    // mHandles is not empty for the first time the buffer is passed to VDA. In that case, VDA needs
    // to import the buffer first.
    if (!info->mFdHaveSet) {
        uint8_t* vaddr = NULL;
        uint32_t size = 0;
        bool isNV21 = true;
        int metaFd =-1;
        if (mVideoDecWraper) {
            mVideoDecWraper->importBufferForPicture(info->mBlockId, info->mFd,
                    metaFd, vaddr, size, isNV21);
            info->mFdHaveSet = true;
            ALOGV("%s fd:%d, id:%d, usecount:%ld", __func__, info->mFd, info->mBlockId, info->mGraphicBlock.use_count());
        }
    } else {
        if (mVideoDecWraper) {
            mVideoDecWraper->reusePictureBuffer(info->mBlockId);
        }
    }
}

bool C2VDAComponent::parseCodedColorAspects(const C2ConstLinearBlock& input) {
    UNUSED(input);
#if 0
    C2ReadView view = input.map().get();
    const uint8_t* data = view.data();
    const uint32_t size = view.capacity();
    std::unique_ptr<media::H264Parser> h264Parser = std::make_unique<media::H264Parser>();
    h264Parser->SetStream(data, static_cast<off_t>(size));
    media::H264NALU nalu;
    media::H264Parser::Result parRes = h264Parser->AdvanceToNextNALU(&nalu);
    if (parRes != media::H264Parser::kEOStream && parRes != media::H264Parser::kOk) {
        ALOGE("H264 AdvanceToNextNALU error: %d", static_cast<int>(parRes));
        return false;
    }
    if (nalu.nal_unit_type != media::H264NALU::kSPS) {
        ALOGV("NALU is not SPS");
        return false;
    }

    int spsId;
    parRes = h264Parser->ParseSPS(&spsId);
    if (parRes != media::H264Parser::kEOStream && parRes != media::H264Parser::kOk) {
        ALOGE("H264 ParseSPS error: %d", static_cast<int>(parRes));
        return false;
    }

    // Parse ISO color aspects from H264 SPS bitstream.
    const media::H264SPS* sps = h264Parser->GetSPS(spsId);
    if (!sps->colour_description_present_flag) {
        ALOGV("No Color Description in SPS");
        return false;
    }
    int32_t primaries = sps->colour_primaries;
    int32_t transfer = sps->transfer_characteristics;
    int32_t coeffs = sps->matrix_coefficients;
    bool fullRange = sps->video_full_range_flag;

    // Convert ISO color aspects to ColorUtils::ColorAspects.
    ColorAspects colorAspects;
    ColorUtils::convertIsoColorAspectsToCodecAspects(primaries, transfer, coeffs, fullRange,
                                                     colorAspects);
    ALOGV("Parsed ColorAspects from bitstream: (R:%d, P:%d, M:%d, T:%d)", colorAspects.mRange,
          colorAspects.mPrimaries, colorAspects.mMatrixCoeffs, colorAspects.mTransfer);

    // Map ColorUtils::ColorAspects to C2StreamColorAspectsInfo::input parameter.
    C2StreamColorAspectsInfo::input codedAspects = {0u};
    if (!C2Mapper::map(colorAspects.mPrimaries, &codedAspects.primaries)) {
        codedAspects.primaries = C2Color::PRIMARIES_UNSPECIFIED;
    }
    if (!C2Mapper::map(colorAspects.mRange, &codedAspects.range)) {
        codedAspects.range = C2Color::RANGE_UNSPECIFIED;
    }
    if (!C2Mapper::map(colorAspects.mMatrixCoeffs, &codedAspects.matrix)) {
        codedAspects.matrix = C2Color::MATRIX_UNSPECIFIED;
    }
    if (!C2Mapper::map(colorAspects.mTransfer, &codedAspects.transfer)) {
        codedAspects.transfer = C2Color::TRANSFER_UNSPECIFIED;
    }
    // Configure to interface.
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    c2_status_t status = mIntfImpl->config({&codedAspects}, C2_MAY_BLOCK, &failures);
    if (status != C2_OK) {
        ALOGE("Failed to config color aspects to interface, error: %d", status);
        return false;
    }
 #endif
    return true;
}

c2_status_t C2VDAComponent::updateColorAspects() {
    ALOGV("updateColorAspects");
    std::unique_ptr<C2StreamColorAspectsInfo::output> colorAspects =
            std::make_unique<C2StreamColorAspectsInfo::output>(
                    0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                    C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED);
    c2_status_t status = mIntfImpl->query({colorAspects.get()}, {}, C2_DONT_BLOCK, nullptr);
    if (status != C2_OK) {
        ALOGE("Failed to query color aspects, error: %d", status);
        return status;
    }
    mCurrentColorAspects = std::move(colorAspects);
    return C2_OK;
}

c2_status_t C2VDAComponent::updateHDRStaticInfo() {
    ALOGV("updateHDRStaticInfo");
    std::unique_ptr<C2StreamHdrStaticInfo::output> hdr =
        std::make_unique<C2StreamHdrStaticInfo::output>();
    c2_status_t err = mIntfImpl->query({hdr.get()}, {}, C2_DONT_BLOCK, nullptr);
    if (err != C2_OK) {
        ALOGE("Failed to query hdr static info, error: %d", err);
        return err;
    }
    mCurrentHdrStaticInfo = std::move(hdr);
    return C2_OK;
}
void C2VDAComponent::updateHDR10PlusInfo() {
    ALOGV("updateHDR10PlusInfo");
    std::string hdr10Data;
    if (mMetaDataUtil->getHDR10PlusData(hdr10Data)) {
        if (hdr10Data.size() != 0) {
            std::memcpy(mCurrentHdr10PlusInfo->m.value, hdr10Data.c_str(), hdr10Data.size());
            mCurrentHdr10PlusInfo->setFlexCount(hdr10Data.size());
            ALOGV("get HDR10Plus data size:%d ", hdr10Data.size());
        }
    }
}

void C2VDAComponent::onVisibleRectChanged(const media::Rect& cropRect) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("onVisibleRectChanged");
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    // We should make sure there is no pending output format change. That is, the input cropRect is
    // corresponding to current output format.
    CHECK(mPendingOutputFormat == nullptr);
    setOutputFormatCrop(cropRect);
}

void C2VDAComponent::setOutputFormatCrop(const media::Rect& cropRect) {
    ALOGV("setOutputFormatCrop(%dx%d)", cropRect.width(), cropRect.height());
    // This visible rect should be set as crop window for each C2ConstGraphicBlock passed to
    // framework.
    mOutputFormat.mVisibleRect = cropRect;
}

void C2VDAComponent::onCheckVideoDecReconfig() {
    ALOGV("%s",__func__);
    if (mSurfaceUsageGeted)
        return;

    if (mBlockPoolUtil == nullptr) {
        std::shared_ptr<C2BlockPool> blockPool;
        auto poolId = mIntfImpl->getBlockPoolId();
        auto err = GetCodec2BlockPool(poolId, shared_from_this(), &blockPool);
        if (err != C2_OK || !blockPool) {
            ALOGI("get block pool ok, id:%lld", poolId);
            err = CreateCodec2BlockPool(poolId, shared_from_this(), &blockPool);
            if (err != C2_OK) {
                ALOGE("Graphic block allocator is invalid");
                reportError(err);
            }
        }
        mUseBufferQueue = blockPool->getAllocatorId() == C2PlatformAllocatorStore::BUFFERQUEUE;
        mBlockPoolUtil = std::make_shared<C2VDABlockPoolUtil> (blockPool);
        if (mBlockPoolUtil->isBufferQueue()) {
            ALOGI("Bufferqueue-backed block pool is used. blockPool->getAllocatorId() %d, C2PlatformAllocatorStore::BUFFERQUEUE %d",
                  blockPool->getAllocatorId(), C2PlatformAllocatorStore::BUFFERQUEUE);
        } else {
            ALOGI("Bufferpool-backed block pool is used.");
        }
    }
    if (mBlockPoolUtil->isBufferQueue()) {
        bool usersurfacetexture = false;
        uint64_t usage = 0;
        usage = mBlockPoolUtil->getConsumerUsage();
        if (!(usage & GRALLOC_USAGE_HW_COMPOSER)) {
            usersurfacetexture = true;
        }

        if (usersurfacetexture) {
            if (mVideoDecWraper) {
                mVideoDecWraper->destroy();
            } else {
                mVideoDecWraper = std::make_shared<VideoDecWraper>();
            }
            mMetaDataUtil->setUseSurfaceTexture(usersurfacetexture);
            mMetaDataUtil->codecConfig(&mConfigParam);
            uint32_t vdecflags = AM_VIDEO_DEC_INIT_FLAG_CODEC2;
            if (mIntfImpl) {
                if (mIntfImpl->mVdecWorkMode->value == VDEC_STREAMMODE)
                    vdecflags |= AM_VIDEO_DEC_INIT_FLAG_STREAMMODE;
                if (mIntfImpl->mDataSourceType->value == DATASOURCE_DMX)
                    vdecflags |= AM_VIDEO_DEC_INIT_FLAG_DMXDATA_SOURCE;
            }
            mVDAInitResult = (VideoDecodeAcceleratorAdaptor::Result)mVideoDecWraper->initialize(VideoCodecProfileToMime(mIntfImpl->getCodecProfile()),
                        (uint8_t*)&mConfigParam, sizeof(mConfigParam), mSecureMode, this, vdecflags);
        }
    } else {
        //use BUFFERPOOL, no surface
        if (mVideoDecWraper) {
            mVideoDecWraper->destroy();
        } else {
            mVideoDecWraper = std::make_shared<VideoDecWraper>();
        }
        if (isNonTunnelMode()) {
            mMetaDataUtil->setNoSurface(true);
        }
        mMetaDataUtil->codecConfig(&mConfigParam);
        uint32_t vdecflags = AM_VIDEO_DEC_INIT_FLAG_CODEC2;
        if (mIntfImpl) {
            if (mIntfImpl->mVdecWorkMode->value == VDEC_STREAMMODE)
                vdecflags |= AM_VIDEO_DEC_INIT_FLAG_STREAMMODE;
            if (mIntfImpl->mDataSourceType->value == DATASOURCE_DMX)
                vdecflags |= AM_VIDEO_DEC_INIT_FLAG_DMXDATA_SOURCE;
        }
        mVDAInitResult = (VideoDecodeAcceleratorAdaptor::Result)mVideoDecWraper->initialize(VideoCodecProfileToMime(mIntfImpl->getCodecProfile()),
                  (uint8_t*)&mConfigParam, sizeof(mConfigParam), mSecureMode, this, vdecflags);
    }

    mSurfaceUsageGeted = true;
}

c2_status_t C2VDAComponent::queue_nb(std::list<std::unique_ptr<C2Work>>* const items) {
    if (mState.load() != State::RUNNING) {
        ALOGI("queue_nb State[%d].", mState.load());
        return C2_BAD_STATE;
    }

    if (!mSurfaceUsageGeted) {
        mTaskRunner->PostTask(FROM_HERE,
                        ::base::Bind(&C2VDAComponent::onCheckVideoDecReconfig, ::base::Unretained(this)));
    }

    while (!items->empty()) {
        mHasQueuedWork = true;
        mTaskRunner->PostTask(FROM_HERE,
                              ::base::Bind(&C2VDAComponent::onQueueWork, ::base::Unretained(this),
                                           ::base::Passed(&items->front())));
        items->pop_front();
    }
    return C2_OK;
}

c2_status_t C2VDAComponent::announce_nb(const std::vector<C2WorkOutline>& items) {
    UNUSED(items);
    return C2_OMITTED;  // Tunneling is not supported by now
}

c2_status_t C2VDAComponent::flush_sm(flush_mode_t mode,
                                     std::list<std::unique_ptr<C2Work>>* const flushedWork) {
    if (mode != FLUSH_COMPONENT) {
        return C2_OMITTED;  // Tunneling is not supported by now
    }
    if (mState.load() != State::RUNNING) {
        return C2_BAD_STATE;
    }
    if (!mHasQueuedWork) {
        return C2_OK;
    }
    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::onFlush,
                                                  ::base::Unretained(this)));
    // Instead of |flushedWork|, abandoned works will be returned via onWorkDone_nb() callback.
    return C2_OK;
}

c2_status_t C2VDAComponent::drain_nb(drain_mode_t mode) {
    C2VDA_LOG(CODEC2_LOG_INFO, "%s drain mode:%d", __func__, mode);
    if (mode != DRAIN_COMPONENT_WITH_EOS && mode != DRAIN_COMPONENT_NO_EOS) {
        return C2_OMITTED;  // Tunneling is not supported by now
    }
    if (mState.load() != State::RUNNING) {
        return C2_BAD_STATE;
    }
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onDrain, ::base::Unretained(this),
                                       static_cast<uint32_t>(mode)));
    return C2_OK;
}

c2_status_t C2VDAComponent::start() {
    // Use mStartStopLock to block other asynchronously start/stop calls.
    ALOGV("%s",__func__);
    std::lock_guard<std::mutex> lock(mStartStopLock);

    if (mState.load() != State::LOADED) {
        return C2_BAD_STATE;  // start() is only supported when component is in LOADED state.
    }

    mCodecProfile = mIntfImpl->getCodecProfile();
    ALOGV("get parameter: mCodecProfile = %d", static_cast<int>(mCodecProfile));

    ::base::WaitableEvent done(::base::WaitableEvent::ResetPolicy::AUTOMATIC,
                               ::base::WaitableEvent::InitialState::NOT_SIGNALED);
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onStart, ::base::Unretained(this),
                                       mCodecProfile, &done));
    done.Wait();
    c2_status_t c2Status;
    if (mVDAInitResult == VideoDecodeAcceleratorAdaptor::Result::PLATFORM_FAILURE) {
        // Regard unexpected VDA initialization failure as no more resources, because we still don't
        // have a formal way to obtain the max capable number of concurrent decoders.
        c2Status = C2_NO_MEMORY;
    } else {
        c2Status = adaptorResultToC2Status(mVDAInitResult);
    }

    if (c2Status != C2_OK) {
        ALOGE("Failed to start component due to VDA error...");
        return c2Status;
    }
    mState.store(State::RUNNING);
    mVDAComponentStopDone = false;
    ALOGV("%s done",__func__);
    return C2_OK;
}

// Stop call should be valid in all states (even in error).
c2_status_t C2VDAComponent::stop() {
    ALOGV("%s",__func__);
    // Use mStartStopLock to block other asynchronously start/stop calls.
    std::lock_guard<std::mutex> lock(mStartStopLock);

    if (mVDAComponentStopDone) {
        return C2_CANNOT_DO;
    }

    auto state = mState.load();
    if (!(state == State::RUNNING || state == State::ERROR)) {
        return C2_OK;  // Component is already in stopped state.
    }

    ::base::WaitableEvent done(::base::WaitableEvent::ResetPolicy::AUTOMATIC,
                               ::base::WaitableEvent::InitialState::NOT_SIGNALED);
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onStop, ::base::Unretained(this), &done));
    done.Wait();
    mState.store(State::LOADED);
    mVDAComponentStopDone = true;
    ALOGV("%s done",__func__);
    return C2_OK;
}

c2_status_t C2VDAComponent::reset() {
    ALOGV("%s",__func__);
    mVDAComponentStopDone = false;
    return stop();
    // TODO(johnylin): reset is different than stop that it could be called in any state.
    // TODO(johnylin): when reset is called, set ComponentInterface to default values.
}

c2_status_t C2VDAComponent::release() {
    ALOGV("%s",__func__);
    return reset();
}

std::shared_ptr<C2ComponentInterface> C2VDAComponent::intf() {
    return mIntf;
}

void C2VDAComponent::ProvidePictureBuffers(uint32_t minNumBuffers, uint32_t width, uint32_t height) {
    // Always use fexible pixel 420 format YCbCr_420_888 in Android.
    // Uses coded size for crop rect while it is not available.
    if (mBufferFirstAllocated && minNumBuffers < mOutputFormat.mMinNumBuffers)
        minNumBuffers = mOutputFormat.mMinNumBuffers;

    uint32_t max_width = width;
    uint32_t max_height = height;

    if (!mMetaDataUtil->getNeedReallocBuffer()) {
        mMetaDataUtil->getMaxBufWidthAndHeight(&max_width, &max_height);
    }
    auto format = std::make_unique<VideoFormat>(HalPixelFormat::YCRCB_420_SP, minNumBuffers,
                                                media::Size(max_width, max_height), media::Rect(width, height));

    // Set mRequestedVisibleRect to default.
    mRequestedVisibleRect = media::Rect();

    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::onOutputFormatChanged,
                                                  ::base::Unretained(this),
                                                  ::base::Passed(&format)));
}

void C2VDAComponent::DismissPictureBuffer(int32_t pictureBufferId) {
    UNUSED(pictureBufferId);
    // no ops
}

void C2VDAComponent::PictureReady(int32_t pictureBufferId, int64_t bitstreamId,
                                  uint32_t x, uint32_t y, uint32_t w, uint32_t h, int32_t flags) {
    UNUSED(pictureBufferId);
    UNUSED(bitstreamId);

    if (mRequestedVisibleRect != media::Rect(x, y, w, h)) {
        mRequestedVisibleRect = media::Rect(x, y, w, h);
        mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::onVisibleRectChanged,
                                                      ::base::Unretained(this), media::Rect(x, y, w, h)));
    }

    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::onOutputBufferDone,
                                                  ::base::Unretained(this),
                                                  pictureBufferId, bitstreamId, flags));
}

void C2VDAComponent::UpdateDecInfo(const uint8_t* info, uint32_t isize) {
    UNUSED(info);
    UNUSED(isize);
    struct aml_dec_params* pinfo = (struct aml_dec_params*)info;
    ALOGV("C2VDAComponent::UpdateDecInfo, dec_parms_status=%d\n", pinfo->parms_status);
    mMetaDataUtil->updateDecParmInfo(pinfo);
}


void C2VDAComponent::NotifyEndOfBitstreamBuffer(int32_t bitstreamId) {
    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::onInputBufferDone,
                                                  ::base::Unretained(this), bitstreamId));
}

void C2VDAComponent::NotifyFlushDone() {
    ALOGI("EOS NotifyFlushDone");
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onDrainDone, ::base::Unretained(this)));
}

void C2VDAComponent::NotifyFlushOrStopDone() {
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onFlushOrStopDone, ::base::Unretained(this)));
}

void C2VDAComponent::onReportError(c2_status_t error) {
    if (mComponentState == ComponentState::DESTROYED) {
        return;
    }
    reportError(error);
}

void C2VDAComponent::NotifyError(int error) {
    ALOGE("Got notifyError from VDA...");
    c2_status_t err = adaptorResultToC2Status((VideoDecodeAcceleratorAdaptor::Result)error);
    if (err == C2_OK) {
        ALOGW("Shouldn't get SUCCESS err code in NotifyError(). Skip it...");
        return;
    }
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VDAComponent::onReportError, ::base::Unretained(this), err));
}

void C2VDAComponent::NotifyEvent(uint32_t event, void *param, uint32_t paramsize) {
    UNUSED(param);
    UNUSED(paramsize);
    switch (event) {
        case VideoDecWraper::FIELD_INTERLACED:
            CODEC2_LOG(CODEC2_LOG_INFO, "is interlaced");
            mMetaDataUtil->updateInterlacedInfo(true);
            break;
        default:
            CODEC2_LOG(CODEC2_LOG_INFO, "NotifyEvent:event:%d", event);
            break;
    }
}

void C2VDAComponent::detectNoShowFrameWorksAndReportIfFinished(
        const C2WorkOrdinalStruct& currOrdinal) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());

    {
        for (auto& work : mPendingWorks) {
            // A work in mPendingWorks would be considered to have no-show frame if there is no
            // corresponding output buffer returned while the one of the work with latter timestamp is
            // already returned. (VDA is outputted in display order.)
            if (isNoShowFrameWork(*(work.get()), currOrdinal)) {
                // Mark FLAG_DROP_FRAME for no-show frame work.
                work->worklets.front()->output.flags = C2FrameData::FLAG_DROP_FRAME;

                // We need to call reportWorkIfFinished() for all detected no-show frame works. However,
                // we should do it after the detection loop since reportWorkIfFinished() may erase
                // entries in mPendingWorks.
                int32_t bitstreamId = frameIndexToBitstreamId(work->input.ordinal.frameIndex);
                mNoShowFrameBitstreamIds.push_back(bitstreamId);
                ALOGD("Detected no-show frame work index=%llu timestamp=%llu",
                      work->input.ordinal.frameIndex.peekull(),
                      work->input.ordinal.timestamp.peekull());
            }
        }
    }
    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::reportWorkForNoShowFrames, ::base::Unretained(this)));
}

void C2VDAComponent::reportWorkForNoShowFrames() {
    for (int32_t bitstreamId : mNoShowFrameBitstreamIds) {
        // Try to report works with no-show frame.
        reportWorkIfFinished(bitstreamId,0);
    }
    mNoShowFrameBitstreamIds.clear();
}

bool C2VDAComponent::isNoShowFrameWork(const C2Work& work,
                                       const C2WorkOrdinalStruct& currOrdinal) const {
    // We consider Work contains no-show frame when all conditions meet:
    // 1. Work's ordinal is smaller than current ordinal.
    // 2. Work's output buffer is not returned.
    // 3. Work is not EOS, CSD, or marked with dropped frame.
    bool smallOrdinal = (work.input.ordinal.timestamp < currOrdinal.timestamp) &&
                        (work.input.ordinal.frameIndex < currOrdinal.frameIndex);
    bool outputReturned = !work.worklets.front()->output.buffers.empty();
    bool specialWork = (work.input.flags & C2FrameData::FLAG_END_OF_STREAM) ||
                       (work.input.flags & C2FrameData::FLAG_CODEC_CONFIG) ||
                       (work.worklets.front()->output.flags & C2FrameData::FLAG_DROP_FRAME);
    return smallOrdinal && !outputReturned && !specialWork;
}

void C2VDAComponent::reportEmptyWork(int32_t bitstreamId, int32_t flags) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    auto workIter = findPendingWorkByBitstreamId(bitstreamId);
    if (workIter == mPendingWorks.end()) {
        ALOGD("reportEmptyWork findPendingWorkByBitstreamId failed. bitstreamId:%d",bitstreamId);
        return;
    }
    auto work = workIter->get();
    work->result = C2_OK;
    work->worklets.front()->output.buffers.clear();
    work->workletsProcessed = 1;
    work->input.ordinal.customOrdinal = mMetaDataUtil->getLastOutputPts();
    c2_cntr64_t timestamp = work->worklets.front()->output.ordinal.timestamp + work->input.ordinal.customOrdinal
                            - work->input.ordinal.timestamp;
    ALOGD("reportEmptyWork index=%llu pts=%llu,%d", work->input.ordinal.frameIndex.peekull(), timestamp.peekull(),__LINE__);

    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        ALOGE("reportEmptyWork: This is EOS work and should be processed by reportEOSWork().");
    } else
    if (work->input.buffers.front()) {
        ALOGE("reportEmptyWork:  Input buffer is still owned by VDA.");
    } else
    if (mPendingOutputEOS && mPendingWorks.size() == 1u) {
        ALOGE("reportEmptyWork:  If mPendingOutputEOS is true, the last returned work should be marked EOS flag and returned by reportEOSWork() instead.");
    }

    std::list<std::unique_ptr<C2Work>> finishedWorks;
    finishedWorks.emplace_back(std::move(*workIter));
    mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
    mPendingWorks.erase(workIter);
}

void C2VDAComponent::reportWorkIfFinished(int32_t bitstreamId, int32_t flags, bool isEmptyWork) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());

    auto workIter = findPendingWorkByBitstreamId(bitstreamId);
    if (workIter == mPendingWorks.end()) {
        ALOGE("%s:%d can not find work with bistreamid:%d", __func__, __LINE__, bitstreamId);
        reportError(C2_CORRUPTED);
        return;
    }

    // EOS work will not be reported here. reportEOSWork() does it.
    auto work = workIter->get();
    if (isEmptyWork || isWorkDone(work)) {
        if (work->worklets.front()->output.flags & C2FrameData::FLAG_DROP_FRAME) {
            // A work with neither flags nor output buffer would be treated as no-corresponding
            // output by C2 framework, and regain pipeline capacity immediately.
            // TODO(johnylin): output FLAG_DROP_FRAME flag after it could be handled correctly.
            work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(0);
        }
        work->result = C2_OK;
        work->workletsProcessed = static_cast<uint32_t>(work->worklets.size());
        work->input.ordinal.customOrdinal = mMetaDataUtil->checkAndAdjustOutPts(work, flags);
        c2_cntr64_t timestamp = work->worklets.front()->output.ordinal.timestamp + work->input.ordinal.customOrdinal
                                - work->input.ordinal.timestamp;
        ALOGV("Reported finished work index=%llu pts=%llu,%d", work->input.ordinal.frameIndex.peekull(), timestamp.peekull(),__LINE__);
        std::list<std::unique_ptr<C2Work>> finishedWorks;
        finishedWorks.emplace_back(std::move(*workIter));
        mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
        mPendingWorks.erase(workIter);
    }
}

bool C2VDAComponent::isWorkDone(const C2Work* work) const {
    if (work->input.buffers.front()) {
        // Input buffer is still owned by VDA.
        return false;
    }

    if (mPendingOutputEOS && mPendingWorks.size() == 1u) {
        // If mPendingOutputEOS is true, the last returned work should be marked EOS flag and
        // returned by reportEOSWork() instead.
        return false;
    }

    if (!(work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) &&
            !(work->worklets.front()->output.flags & C2FrameData::FLAG_DROP_FRAME)) {
        // Unless the input is CSD or the output is dropped, this work is not done because the
        // output buffer is not returned from VDA yet.
        // tunnel mode need add rendertime info update
        if (isNonTunnelMode()) {
            if (work->worklets.front()->output.buffers.empty()) {
                return false;
            }
        } else if (isTunnelMode()) {
            if (work->worklets.front()->output.configUpdate.empty()) {
                return false;
            } else {
                auto existingParm = std::find_if(
                    work->worklets.front()->output.configUpdate.begin(), work->worklets.front()->output.configUpdate.end(),
                    [index = C2PortTunnelSystemTime::CORE_INDEX](const std::unique_ptr<C2Param>& param) { return param->coreIndex().coreIndex() == index; });
                if (existingParm == work->worklets.front()->output.configUpdate.end()) {
                    return false;
                }
            }
        }
    }
    return true;  // This work is done.
}

bool C2VDAComponent::isNonTunnelMode() const {
    return (mSyncType == C2_SYNC_TYPE_NON_TUNNEL);
}

bool C2VDAComponent::isTunnelMode() const {
    return (mSyncType == C2_SYNC_TYPE_TUNNEL);
}

bool C2VDAComponent::isTunnerPassthroughMode() const {
    return (mSyncType == (C2_SYNC_TYPE_TUNNEL | C2_SYNC_TYPE_PASSTHROUTH));
}

c2_status_t C2VDAComponent::reportEOSWork() {
    ALOGI("reportEOSWork");
    DCHECK(mTaskRunner->BelongsToCurrentThread());

    if (mPendingWorks.empty()) {
        ALOGE("Failed to find EOS work.");
        reportError(C2_CORRUPTED);
        return C2_CORRUPTED;
    }

    mPendingOutputEOS = false;
    std::unique_ptr<C2Work> eosWork = std::move(mPendingWorks.back());
    mPendingWorks.pop_back();
    if (!eosWork->input.buffers.empty()) {
        eosWork->input.buffers.front().reset();
    }
    eosWork->result = C2_OK;
    eosWork->workletsProcessed = static_cast<uint32_t>(eosWork->worklets.size());
    eosWork->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;


    if (!mPendingWorks.empty()) {
        ALOGW("There are remaining works except EOS work. abandon them.");
        for (const auto& kv : mPendingWorks) {
            ALOGW("Work index=%llu, timestamp=%llu",
                  kv->input.ordinal.frameIndex.peekull(),
                  kv->input.ordinal.timestamp.peekull());
        }
        reportAbandonedWorks();
    }

    std::list<std::unique_ptr<C2Work>> finishedWorks;
    finishedWorks.emplace_back(std::move(eosWork));
    mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
    return C2_OK;
}

void C2VDAComponent::reportAbandonedWorks() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    std::list<std::unique_ptr<C2Work>> abandonedWorks;

    while (!mPendingWorks.empty()) {
        std::unique_ptr<C2Work> work(std::move(mPendingWorks.front()));
        mPendingWorks.pop_front();

        // TODO: correlate the definition of flushed work result to framework.
        work->result = C2_NOT_FOUND;
        // When the work is abandoned, buffer in input.buffers shall reset by component.
        if (!work->input.buffers.empty()) {
            work->input.buffers.front().reset();
        }
        if (mTunnelHelper) {
            mTunnelHelper->storeAbandonedFrame(work->input.ordinal.timestamp.peekull());
        }
        C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL2, "%s tunnel mode abandon mediatimeus:%lld", __func__, work->input.ordinal.timestamp.peekull());
        abandonedWorks.emplace_back(std::move(work));
    }

    for (auto& work : mAbandonedWorks) {
        // TODO: correlate the definition of flushed work result to framework.
        work->result = C2_NOT_FOUND;
        // When the work is abandoned, buffer in input.buffers shall reset by component.
        if (!work->input.buffers.empty()) {
            work->input.buffers.front().reset();
        }
        C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL2, "%s tunnel mode abandon mediatimeus:%lld", __func__, work->input.ordinal.timestamp.peekull());
        if (mTunnelHelper) {
            mTunnelHelper->storeAbandonedFrame(work->input.ordinal.timestamp.peekull());
        }
        abandonedWorks.emplace_back(std::move(work));
    }
    mAbandonedWorks.clear();

    // Pending EOS work will be abandoned here due to component flush if any.
    mPendingOutputEOS = false;

    if (!abandonedWorks.empty()) {
        mListener->onWorkDone_nb(shared_from_this(), std::move(abandonedWorks));
    }
}

void C2VDAComponent::reportError(c2_status_t error) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    if (mComponentState == ComponentState::DESTROYING ||
        mComponentState == ComponentState::DESTROYED ||
        mComponentState == ComponentState::UNINITIALIZED) {
        C2VDA_LOG(CODEC2_LOG_DEBUG_LEVEL1, "%s have been in destrory or stop state", __func__);
        return;
    }
    mListener->onError_nb(shared_from_this(), static_cast<uint32_t>(error));
    mHasError = true;
    mState.store(State::ERROR);
}

bool C2VDAComponent::startDequeueThread(const media::Size& size, uint32_t pixelFormat,
                                        bool resetBuffersInClient) {
    CHECK(!mDequeueThread.IsRunning());
    if (!mDequeueThread.Start()) {
        ALOGE("failed to start dequeue thread!!");
        return false;
    }
    mDequeueLoopStop.store(false);

    if (!mBufferFirstAllocated) {
        mDequeueLoopRunning.store(false);
    }

    if (resetBuffersInClient) {
        mBuffersInClient.store(0u);
    }
    mDequeueThread.task_runner()->PostTask(
            FROM_HERE, ::base::Bind(&C2VDAComponent::dequeueThreadLoop, ::base::Unretained(this),
                                    size, pixelFormat));
    return true;
}

void C2VDAComponent::stopDequeueThread() {
    if (mDequeueThread.IsRunning()) {
        mDequeueLoopStop.store(true);
        mDequeueThread.Stop();
    }
}

int32_t C2VDAComponent::getFetchGraphicBlockDelayTimeUs(c2_status_t err) {
    // Variables used to exponential backoff retry when buffer fetching times out.
    constexpr int kFetchRetryDelayInit = 64;    // Initial delay: 64us
    int kFetchRetryDelayMax = DEFAULT_FRAME_DURATION;
    float frameRate = 0.0f;
    int perFrameDur = 0;
    static int sDelay = kFetchRetryDelayInit;

    if (mIntfImpl)
        frameRate = mIntfImpl->getInputFrameRate();
    if (frameRate > 0.0f) {
        perFrameDur = (int) (1000 / frameRate) * 1000;
        if (mMetaDataUtil->isInterlaced())
            perFrameDur = perFrameDur / 2;
        perFrameDur = std::max(perFrameDur, 2 * kFetchRetryDelayInit);
        kFetchRetryDelayMax = std::min(perFrameDur, kFetchRetryDelayMax);
    }
    if (err == C2_TIMED_OUT || err == C2_BLOCKING) {
        ALOGV("%s: fetchGraphicBlock() timeout, waiting %zuus frameRate:%f", __func__, sDelay, frameRate);
        sDelay = std::min(sDelay * 2, kFetchRetryDelayMax);  // Exponential backoff
        return sDelay;
    }
    sDelay = kFetchRetryDelayInit;
    return sDelay;
}

void C2VDAComponent::dequeueThreadLoop(const media::Size& size, uint32_t pixelFormat) {
    ALOGV("dequeueThreadLoop starts");
    DCHECK(mDequeueThread.task_runner()->BelongsToCurrentThread());
    uint64_t platformUsage = mMetaDataUtil->getPlatformUsage();
    int      delayTime = 10 * 1000;//10ms
    C2MemoryUsage usage = {
            mSecureMode ? (C2MemoryUsage::READ_PROTECTED | C2MemoryUsage::WRITE_PROTECTED) :
            (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE),  platformUsage};
    ALOGV("%s usage %llx size(%d*%d)report out put work num:%lld", __func__, usage.expected, size.width(), size.height(),mOutputWorkCount);
    uint64_t timeOutCount = 0;
    uint64_t lastFetchBlockTimeMs = 0;
    uint64_t timeOutMs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000000;
    while (!mDequeueLoopStop.load()) {

        if (mBuffersInClient.load() == 0) {
            ::usleep(kDequeueRetryDelayUs);  // wait for retry
            //continue;
        }

        uint64_t nowTimeMs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000000;
        //we reuse this to check reset is complete or not it timeout will force reset once time
        if (mComponentState == ComponentState::FLUSHING &&
            mLastFlushTimeMs > 0 && nowTimeMs - mLastFlushTimeMs >= 2000) {
            ALOGI("onFlush timeout we need force flush once time");
            mLastFlushTimeMs = 0;
            mVideoDecWraper->stop(RESET_FLAG_NOWAIT);
        } else if (mComponentState == ComponentState::STOPPING) {
            ALOGV("%s@%d", __func__, __LINE__);
            break;
        }
        // wait for running
        if (mDequeueLoopRunning.load()) {
            uint32_t blockId = 0;
            c2_status_t err = C2_TIMED_OUT;
            C2BlockPool::local_id_t poolId;
            std::shared_ptr<C2GraphicBlock> block;

            if (mMetaDataUtil != NULL && mBlockPoolUtil != NULL) {
                mBlockPoolUtil->getPoolId(&poolId);
                err = mBlockPoolUtil->fetchGraphicBlock(mMetaDataUtil->getOutAlignedSize(size.width()),
                                                mMetaDataUtil->getOutAlignedSize(size.height()),
                                                pixelFormat, usage, &block);
                ALOGI("dequeueThreadLoop fetchOutputBlock %d state:%d", __LINE__, err);
            }
            delayTime = getFetchGraphicBlockDelayTimeUs(err);
            if (err == C2_TIMED_OUT) {
                // Mutexes often do not care for FIFO. Practically the thread who is locking the mutex
                // usually will be granted to lock again right thereafter. To make this loop not too
                // bossy, the simpliest way is to add a short delay to the next time acquiring the
                // lock. TODO (b/118354314): replace this if there is better solution.
                ::usleep(delayTime);
                continue;  // wait for retry
            }
            if (err == C2_OK) {
                mBlockPoolUtil->getBlockIdByGraphicBlock(block,&blockId);
                GraphicBlockInfo *info = getGraphicBlockByBlockId(poolId, blockId);
                bool block_used = false;
                int width = block->width();
                int height = block->height();
                int use_count = block.use_count();
                if (info == nullptr) { //fetch unused block
                    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VDAComponent::onNewBlockBufferFetched,
                                        ::base::Unretained(this), std::move(block), poolId, blockId));
                } else { //fetch used block
                    mTaskRunner->PostTask(FROM_HERE,
                                    ::base::Bind(&C2VDAComponent::onOutputBufferReturned,
                                                ::base::Unretained(this), std::move(block), poolId, blockId));
                    mBuffersInClient--;
                    block_used = true;
                }
                nowTimeMs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000000;
                ALOGV("dequeueThreadLoop fetch %s block(id:%d,w:%d h:%d,count:%d), time interval:%lld",
                        (block_used ? "used" : "unused"), blockId, width, height, use_count,
                        nowTimeMs - lastFetchBlockTimeMs);
                timeOutCount = 0;
                lastFetchBlockTimeMs = nowTimeMs;
            } else if (err == C2_BLOCKING) {
                ::usleep(delayTime);
                if (timeOutCount == 0)
                    timeOutMs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000000;
                timeOutCount++;
                ALOGV("fetch block retry timeout[%lld][%lld][%lld]timeOutCount[%lld].",
                        (systemTime(SYSTEM_TIME_MONOTONIC) / 1000000) - timeOutMs,
                        (systemTime(SYSTEM_TIME_MONOTONIC) / 1000000),
                        timeOutMs,
                        timeOutCount);
                if (((systemTime(SYSTEM_TIME_MONOTONIC) / 1000000) - timeOutMs) > mDefaultRetryBlockTimeOutMs
                    && (mUseBufferQueue == false)) {
                    displayGraphicBlockInfo();
                    reportError(C2_TIMED_OUT);
                    break;
                }
                continue;  // wait for retry
            } else {
                ALOGE("dequeueThreadLoop got error: %d", err);
                //break;
            }
        }
        ::usleep(delayTime);
    }
    ALOGV("dequeueThreadLoop terminates");
}

const char* C2VDAComponent::GraphicBlockState(GraphicBlockInfo::State state) {
    if (state == GraphicBlockInfo::State::OWNED_BY_COMPONENT) {
        return "Component";
    } else if (state == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR) {
        return "Accelerator";
    } else if (state == GraphicBlockInfo::State::OWNED_BY_CLIENT) {
        return "Client";
    } else if (state == GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER) {
        return "Tunnelrender";
    } else {
        return "Unknown";
    }
}

// for debug
void C2VDAComponent::displayGraphicBlockInfo() {
    for (auto & blockItem : mGraphicBlocks) {
        ALOGD("%s PoolId(%d) BlockId(%d) Fd(%d) State(%s) Blind(%d) FdHaveSet(%d) UseCount(%ld)",
             __func__, blockItem.mPoolId, blockItem.mBlockId,
            blockItem.mFd, GraphicBlockState(blockItem.mState),
            blockItem.mBind, blockItem.mFdHaveSet, blockItem.mGraphicBlock.use_count());
    }
}

void C2VDAComponent::getCurrentProcessFdInfo() {
    int iPid = (int)getpid();
    std::string path;
    struct dirent *dir_info;
    path.append("/proc/" + std::to_string(iPid) + "/fdinfo/");
    DIR *dir= opendir(path.c_str());
    dir_info = readdir(dir);
    while (dir_info != NULL) {
        if (strcmp(dir_info->d_name, ".") != 0 && strcmp(dir_info->d_name, "..") != 0) {
            std::string p = path;
            p.append(dir_info->d_name);

            std::ifstream  srcFile(p.c_str(), std::ios::in | std::ios::binary);
            std::string data;
            if (!srcFile.fail()) {
                std::string tmp;
                while (srcFile.peek() != EOF) {
                    srcFile >> tmp;
                    data.append(" " + tmp);
                    tmp.clear();
                }
            }
            srcFile.close();
            ALOGD("%s info: fd(%s) %s", __func__ ,dir_info->d_name, data.c_str());
        }
        dir_info = readdir(dir);
    }
}

const char* C2VDAComponent::VideoCodecProfileToMime(media::VideoCodecProfile profile) {
    if (profile >= media::H264PROFILE_MIN && profile <= media::H264PROFILE_MAX) {
        return "video/avc";
    } else if (profile >= media::HEVCPROFILE_MIN && profile <= media::HEVCPROFILE_MAX) {
        return "video/hevc";
    } else if (profile >= media::VP9PROFILE_MIN && profile <= media::VP9PROFILE_MAX) {
        return "video/x-vnd.on2.vp9";
    } else if (profile >= media::AV1PROFILE_MIN && profile <= media::AV1PROFILE_MAX) {
        return "video/av01";
    } else if (profile >= media::DOLBYVISION_MIN && profile <= media::DOLBYVISION_MAX) {
        return "video/dolby-vision";
    } else if (profile == media::MPEG4_PROFILE) {
        return "video/mp4v-es";
    }  else if (profile == media::MPEG2_PROFILE) {
        return "video/mpeg2";
    }
    return "";
}


void C2VDAComponent::onConfigureTunnelMode() {
    /* configure */
    C2VDA_LOG(CODEC2_LOG_INFO, "[%s] synctype:%d, syncid:%d", __func__, mIntfImpl->mTunnelModeOutput->m.syncType, mIntfImpl->mTunnelModeOutput->m.syncId[0]);
    if (mIntfImpl->mTunnelModeOutput->m.syncType == C2PortTunneledModeTuning::Struct::sync_type_t::AUDIO_HW_SYNC) {
        int syncId = mIntfImpl->mTunnelModeOutput->m.syncId[0];
        if (syncId >= 0) {
            if (((syncId & 0x0000FF00) == 0xFF00)
                || (syncId == 0x0)) {
                mSyncId = syncId;
                mTunnelHelper =  std::make_shared<TunnelModeHelper>(this, mSecureMode);
                mSyncType &= (~C2_SYNC_TYPE_NON_TUNNEL);
                mSyncType |= C2_SYNC_TYPE_TUNNEL;
            }
        }
    }

    return;
}

#if 0
void C2VDAComponent::onConfigureTunnerPassthroughMode() {
    int32_t filterid = mIntfImpl->mVendorTunerHalParam->videoFilterId;

    mTunerPassthroughparams.dmx_id = (filterid >> 16);
    mTunerPassthroughparams.video_pid = (filterid & 0x0000FFFF);
    mTunerPassthroughparams.hw_sync_id = mIntfImpl->mVendorTunerHalParam->hwAVSyncId;

    CODEC2_LOG(CODEC2_LOG_INFO, "[%s] passthrough config,dmxid:%d,vpid:%d,syncid:%d",
        __func__,
        mTunerPassthroughparams.dmx_id,
        mTunerPassthroughparams.video_pid,
        mTunerPassthroughparams.hw_sync_id);

    mSyncType &= (~C2_SYNC_TYPE_NON_TUNNEL);
    mSyncType |= C2_SYNC_TYPE_PASSTHROUTH;
}
#endif

class C2VDAComponentFactory : public C2ComponentFactory {
public:
    C2VDAComponentFactory(C2String decoderName)
          : mDecoderName(decoderName),
            mReflector(std::static_pointer_cast<C2ReflectorHelper>(
                    GetCodec2VDAComponentStore()->getParamReflector())){};

    c2_status_t createComponent(c2_node_id_t id, std::shared_ptr<C2Component>* const component,
                                ComponentDeleter deleter) override {
        *component = C2VDAComponent::create(mDecoderName, id, mReflector, deleter);
        return *component ? C2_OK : C2_NO_MEMORY;
    }
    c2_status_t createInterface(c2_node_id_t id,
                                std::shared_ptr<C2ComponentInterface>* const interface,
                                InterfaceDeleter deleter) override {
        UNUSED(deleter);
        *interface =
                std::shared_ptr<C2ComponentInterface>(new SimpleInterface<C2VDAComponent::IntfImpl>(
                        mDecoderName.c_str(), id,
                        std::make_shared<C2VDAComponent::IntfImpl>(mDecoderName, mReflector)));
        return C2_OK;
    }
    ~C2VDAComponentFactory() override = default;

private:
    const C2String mDecoderName;
    std::shared_ptr<C2ReflectorHelper> mReflector;
};
}  // namespace android


#define CreateC2VDAFactory(type) \
    extern "C" ::C2ComponentFactory* CreateC2VDA##type##Factory(bool secureMode) {\
         ALOGV("create compoment %s secure:%d", #type, secureMode);\
         return secureMode ? new ::android::C2VDAComponentFactory(android::k##type##SecureDecoderName)\
                            :new ::android::C2VDAComponentFactory(android::k##type##DecoderName);\
    }
#define CreateC2VDAClearFactory(type) \
        extern "C" ::C2ComponentFactory* CreateC2VDA##type##Factory(bool secureMode) {\
             ALOGV("create compoment %s secure:%d", #type, secureMode);\
             UNUSED(secureMode);\
             return new ::android::C2VDAComponentFactory(android::k##type##DecoderName);\
        }


#define DestroyC2VDAFactory(type) \
    extern "C" void DestroyC2VDA##type##Factory(::C2ComponentFactory* factory) {\
        delete factory;\
    }

CreateC2VDAFactory(H264)
CreateC2VDAFactory(H265)
CreateC2VDAFactory(VP9)
CreateC2VDAFactory(AV1)
CreateC2VDAFactory(DVHE)
CreateC2VDAFactory(DVAV)
CreateC2VDAFactory(DVAV1)
CreateC2VDAClearFactory(MP2V)
CreateC2VDAClearFactory(MP4V)
CreateC2VDAClearFactory(MJPG)


DestroyC2VDAFactory(H264)
DestroyC2VDAFactory(H265)
DestroyC2VDAFactory(VP9)
DestroyC2VDAFactory(AV1)
DestroyC2VDAFactory(DVHE)
DestroyC2VDAFactory(DVAV)
DestroyC2VDAFactory(DVAV1)
DestroyC2VDAFactory(MP2V)
DestroyC2VDAFactory(MP4V)
DestroyC2VDAFactory(MJPG)
