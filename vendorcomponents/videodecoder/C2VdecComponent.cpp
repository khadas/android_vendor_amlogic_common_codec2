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
#define LOG_TAG "C2VdecComponent"

#define __C2_GENERATE_GLOBAL_VARS__


#include <utils/Trace.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <fstream>
#include <string.h>
#include <algorithm>
#include <string>
#include <base/bind.h>
#include <base/bind_helpers.h>
#include <android/hardware/graphics/common/1.0/types.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <media/stagefright/foundation/hexdump.h>
#include <ui/GraphicBuffer.h>
#include <cutils/properties.h>
#include <cutils/native_handle.h>
#include <utils/Log.h>
#include <utils/misc.h>
#include <hardware/gralloc1.h>
#include <C2VdecComponent.h>
#include <C2Buffer.h>
#include <C2AllocatorGralloc.h>
#include <C2ComponentFactory.h>
#include <C2Config.h>
#include <C2PlatformSupport.h>
#include <Codec2Mapper.h>
#include <C2VdecInterfaceImpl.h>
#include <C2VdecDeviceUtil.h>
#include <C2VdecDequeueThreadUtil.h>
#include <C2VdecTunnelHelper.h>
#include <C2VdecTunerPassthroughHelper.h>
#include <C2VdecDebugUtil.h>
#include <C2VendorProperty.h>
#include <c2logdebug.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <linux/videodev2.h>



#ifdef ATRACE_TAG
#undef ATRACE_TAG
#define ATRACE_TAG ATRACE_TAG_VIDEO
#endif

#define AM_SIDEBAND_HANDLE_NUM_FD (0)

#define CODEC_OUTPUT_BUFS_ALIGN_64 64

#define DEFAULT_FRAME_DURATION (16384)// default dur: 16ms (1 frame at 60fps)
#define DEFAULT_RETRYBLOCK_TIMEOUT_MS (60*1000)// default timeout 1min
#define MAX_INSTANCE_LOW_RAM 4
#define MAX_INSTANCE_DEFAULT 9
#define MAX_INSTANCE_SECURE_LOW_RAM 1
#define MAX_INSTANCE_SECURE_DEFAULT 2
#define MAX_INSTANCE_HIGH_RES_DEFAULT 2

#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

#define C2Vdec_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, mSessionID, mDecoderID, ##str)

#define CODEC2_VDEC_ATRACE(tag_name, num) \
do { \
    if (sizeof(num) == sizeof(uint32_t)) { \
        CODEC2_ATRACE_INT32(tag_name, num); \
    } else if (sizeof(num) == sizeof(uint64_t)) { \
        CODEC2_ATRACE_INT64(tag_name, num); \
    } \
} while(0)

uint32_t android::C2VdecComponent::mInstanceNum = 0;
uint32_t android::C2VdecComponent::mInstanceID = 0;

using android::hardware::graphics::common::V1_0::BufferUsage;

namespace android {

namespace {


// Mask against 30 bits to avoid (undefined) wraparound on signed integer.
int32_t frameIndexToBitstreamId(c2_cntr64_t frameIndex) {
    return static_cast<int32_t>(frameIndex.peeku() & 0x3FFFFFFF);
}

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

C2VdecComponent::VideoFormat::VideoFormat(HalPixelFormat pixelFormat, uint32_t minNumBuffers,
                                         media::Size codedSize, media::Rect visibleRect)
      : mPixelFormat(pixelFormat),
        mMinNumBuffers(minNumBuffers),
        mCodedSize(codedSize),
        mVisibleRect(visibleRect) {}


// static
std::atomic<int32_t> C2VdecComponent::sConcurrentInstances = 0;
std::atomic<int32_t> C2VdecComponent::sConcurrentInstanceSecures = 0;
std::atomic<int32_t> C2VdecComponent::sConcurrentMaxResolutionInstance = 0;
std::atomic<int32_t> C2VdecComponent::sConcurrentVc1Instance = 0;




// static
std::shared_ptr<C2Component> C2VdecComponent::create(
        const std::string& name, c2_node_id_t id, const std::shared_ptr<C2ReflectorHelper>& helper,
        C2ComponentFactory::ComponentDeleter deleter) {
    UNUSED(deleter);
    bool isLowMemDevice = !property_get_bool(PROPERTY_PLATFORM_SUPPORT_4K, true);
    int maxInstance = isLowMemDevice ? MAX_INSTANCE_LOW_RAM : MAX_INSTANCE_DEFAULT;
    int maxInstanceSecure = isLowMemDevice ? MAX_INSTANCE_SECURE_LOW_RAM : MAX_INSTANCE_SECURE_DEFAULT;
    static const int32_t kMaxConcurrentInstances =
            property_get_int32(C2_PROPERTY_VDEC_INST_MAX_NUM, maxInstance);
    static const int32_t kMaxSecureConcurrentInstances =
            property_get_int32(C2_PROPERTY_VDEC_INST_MAX_NUM_SECURE, maxInstanceSecure);
    static const int32_t kMaxHighResConcurrentInstances =
            property_get_int32(C2_PROPERTY_VDEC_INST_MAX_HIGH_RES_NUM, MAX_INSTANCE_HIGH_RES_DEFAULT);
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    bool isSecure = name.find(".secure") != std::string::npos;
    if (isSecure) {
        if (kMaxSecureConcurrentInstances >= 0 && sConcurrentInstanceSecures.load() >= kMaxSecureConcurrentInstances) {
            ALOGW("Reject to Initialize() due to too many secure instances: %d", sConcurrentInstanceSecures.load());
            return nullptr;
        }
        if (kMaxSecureConcurrentInstances >= 0 &&
            ((sConcurrentInstances.load() + sConcurrentInstanceSecures.load()) >= kMaxConcurrentInstances)) {
            ALOGW("Reject to Initialize() due to too many secure and nosecure instances: %d", sConcurrentInstanceSecures.load());
            return nullptr;
        }
    } else {
        if (kMaxConcurrentInstances >= 0 &&
            ((sConcurrentInstances.load() + sConcurrentInstanceSecures.load()) >= kMaxConcurrentInstances)) {
            ALOGW("Reject to Initialize() due to too many instances: %d", sConcurrentInstances.load());
            return nullptr;
        }
        //since vc1 use single mode in decoder now, so only 1 instance can be used
        if (sConcurrentVc1Instance.load() > 0 || ((sConcurrentInstances.load() + sConcurrentInstanceSecures.load()) > 0
            && name.find(".vc1") != std::string::npos)) {
            ALOGW("Reject to Initialize() due to vc1 can only create one instance or can't paly with other vdec, sConcurrentVc1Instance:%d",
                sConcurrentVc1Instance.load());
            return nullptr;
        }
    }

    ALOGW("Initialize() instances: %d", sConcurrentMaxResolutionInstance.load());
    if (kMaxHighResConcurrentInstances >= 0 && (sConcurrentMaxResolutionInstance.load() >= kMaxHighResConcurrentInstances)) {
        ALOGW("Reject to Initialize() due to too many Max instances: %d", sConcurrentMaxResolutionInstance.load());
        return nullptr;
    }

    return std::shared_ptr<C2Component>(new C2VdecComponent(name, id, helper));
}

struct DummyReadView : public C2ReadView {
    DummyReadView() : C2ReadView(C2_NO_INIT) {}
};

void C2VdecComponent::Preempted()
{
    mPreempting = true;
}

bool C2VdecComponent::Preempting()
{
    return mPreempting;
}

void QuitEventFunc(void *classptr)
{
    C2VdecComponent *comp = (C2VdecComponent*)classptr;
    ALOGW("CODEC2-%d was preempted", comp->mSessionID);
    comp->Preempted();
}

C2VdecComponent::C2VdecComponent(C2String name, c2_node_id_t id,
                               const std::shared_ptr<C2ReflectorHelper>& helper):
    mIntfImpl(std::make_shared<IntfImpl>(name, helper)),
    mIntf(std::make_shared<SimpleInterface<IntfImpl>>(name.c_str(), id, mIntfImpl)),
    mThread("C2VdecComponentThread"),
    mWeakThisFactory(this),
    mDefaultDummyReadView(DummyReadView()) {

    Init(name);
    C2Vdec_LOG(CODEC2_LOG_INFO, "Create %s(%s)", __func__, name.c_str());
    //  TODO: the client may need to know if init is Failed.
    if (mIntfImpl->status() != C2_OK) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Component interface init Failed (err code = %d)", mIntfImpl->status());
        return;
    }

    mIntfImpl->setComponent(this);

    if (!mThread.Start()) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Component thread Failed to start.");
        return;
    }

    mTaskRunner = mThread.task_runner();
    mState.store(State::LOADED);

    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    //default 1min
    mDefaultRetryBlockTimeOutMs = (uint64_t)property_get_int32(C2_PROPERTY_VDEC_RETRYBLOCK_TIMEOUT, DEFAULT_RETRYBLOCK_TIMEOUT_MS);
    mFdInfoDebugEnable = property_get_bool(C2_PROPERTY_VDEC_FD_INFO_DEBUG, false);

    bool support_soft_10bit = property_get_bool(C2_PROPERTY_VDEC_SUPPORT_10BIT, true);
    support_soft_10bit = property_get_bool(PROPERTY_PLATFORM_SUPPORT_SOFTWARE_P010, support_soft_10bit);
    bool support_hardware_10bit = property_get_bool(PROPERTY_PLATFORM_SUPPORT_HARDWARE_P010, false);

    mSupport10BitDepth = support_soft_10bit || support_hardware_10bit;
    mDebugUtil = std::make_shared<DebugUtil>();
    addObserver(mDebugUtil, static_cast<int>(mComponentState), mCompHasError);
    mDequeueThreadUtil = std::make_shared<DequeueThreadUtil>();
    addObserver(mDequeueThreadUtil, static_cast<int>(mComponentState), mCompHasError);

    if (mFdInfoDebugEnable && mDebugUtil) {
        mDebugUtil->showCurrentProcessFdInfo();
    }
}

void C2VdecComponent::Init(C2String compName) {
    mSyncType = C2_SYNC_TYPE_NON_TUNNEL;
    mTunnelUnderflow = false;
    mResChStat = C2_RESOLUTION_CHANGE_NONE;
    mInterlacedType = C2_INTERLACED_TYPE_NONE;

    mVdecInitResult = VideoDecodeAcceleratorAdaptor::Result::ILLEGAL_STATE;
    updateComponentState(ComponentState::UNINITIALIZED);
    mCodecProfile = media::VIDEO_CODEC_PROFILE_UNKNOWN;
    mState = State::UNLOADED;

    mDeviceUtil = NULL;
    mOutputDelay = nullptr;
    mVideoDecWraper = NULL;
    mLastOutputReportWork = nullptr;

    // tunnel mode
    mTunnelHelper = NULL;
    mTunerPassthroughHelper = NULL;

    mReportEosWork = false;
    mUseBufferQueue = false;
    mHasQueuedWork = false;
    mIsReportEosWork = false;
    mPendingOutputEOS = false;
    mCanQueueOutBuffer = false;
    mSurfaceUsageGot = false;
    mResolutionChanging = false;
    mPictureSizeChanged = false;
    mBufferFirstAllocated = false;
    mVdecComponentStopDone = false;
    mHDR10PlusMeteDataNeedCheck = false;
    mHaveDrainDone = false;
    mHaveFlushDone = false;
    mFlushDoneWithOutEosWork = false;
    mPreempting = false;

    mPlayerId = 0;
    mUnstable = 0;
    mInputQueueNum = 0;
    mInputBufferNum = 0;
    mInputWorkCount = 0;
    mOutBufferCount = 0;
    mOutputWorkCount = 0;
    mLastFlushTimeMs = 0;
    mInputCSDWorkCount = 0;
    mCurrentPixelFormat = 0;
    mErrorFrameWorkCount = 0;
    mUpdateDurationUsCount = 0;
    mOutputFinishedWorkCount = 0;

    mFirstInputTimestamp = -1;
    mLastOutputBitstreamId = -1;
    mLastFinishedBitstreamId = -1;
    mPendingGraphicBlockBufferId = -1;
    mLastInputTimestamp = 0;

    mSyncId = 0;
    mInstanceNum ++;
    mInstanceID ++;
    mSessionID = -1;
    mDecoderID = -1;
    mName = compName;
    mStopDoneEvent = nullptr;

    mIsMaxResolution = false;

    memset(&mConfigParam, 0, sizeof(mConfigParam));
    memset(mGraphicBlockStateCount, 0, sizeof(mGraphicBlockStateCount));

    mSecureMode = compName.find(".secure") != std::string::npos;
    if (mSecureMode)
        sConcurrentInstanceSecures.fetch_add(1, std::memory_order_relaxed);
    else
        sConcurrentInstances.fetch_add(1, std::memory_order_relaxed);
    if (compName.find(".vc1") != std::string::npos) {
        sConcurrentVc1Instance.fetch_add(1, std::memory_order_relaxed);
    }

    mIsDolbyVision = compName.find(".dolby-vision") != std::string::npos;
    mIsReleasing = false;
}

C2VdecComponent::~C2VdecComponent() {
    C2Vdec_LOG(CODEC2_LOG_INFO, "~C2VdecComponent start");
    updateComponentState(ComponentState::DESTROYING);
    if (mDebugUtil) {
        removeObserver(mDebugUtil);
        mDebugUtil.reset();
        mDebugUtil = NULL;
    }
    if (mThread.IsRunning()) {
        ::base::WaitableEvent done(::base::WaitableEvent::ResetPolicy::AUTOMATIC,
                                   ::base::WaitableEvent::InitialState::NOT_SIGNALED);
        mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onDestroy,
                                ::base::Unretained(this), &done));
        done.Wait();
        mThread.Stop();
    }

    C2Vdec_LOG(CODEC2_LOG_INFO, "~C2VdecComponent done");
    --mInstanceNum;
}

void C2VdecComponent::onDestroy(::base::WaitableEvent* done) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s]", __func__);

    if (mDequeueThreadUtil) {
        removeObserver(mDequeueThreadUtil);
        mDequeueThreadUtil->StopRunDequeueTask();
        mDequeueThreadUtil.reset();
        mDequeueThreadUtil = NULL;
    }

    mPendingOutputFormat.reset();
    mPendingBuffersToWork.clear();
    mSentOutBitStreamIdList.clear();
    mNoOutFrameWorkQueue.clear();

    if (mVideoDecWraper) {
        mVideoDecWraper->destroy();
        mVideoDecWraper.reset();
        mVideoDecWraper = NULL;
    }
    if (mDeviceUtil) {
        removeObserver(mDeviceUtil);
        mDeviceUtil.reset();
        mDeviceUtil = NULL;
    }
    if (mTunerPassthroughHelper) {
        removeObserver(mTunerPassthroughHelper);
        mTunerPassthroughHelper.reset();
        mTunerPassthroughHelper = NULL;
    }
    if (mTunnelHelper) {
        removeObserver(mTunnelHelper);
        mTunnelHelper.reset();
        mTunnelHelper = NULL;
    }

    for (auto& info : mGraphicBlocks) {
        info.mGraphicBlock.reset();
    }

    mGraphicBlocks.clear();
    if (mBlockPoolUtil != NULL) {
        mBlockPoolUtil->cancelAllGraphicBlock();
        mBlockPoolUtil.reset();
        mBlockPoolUtil = NULL;
    }
    if (mLastOutputReportWork != NULL) {
        delete mLastOutputReportWork;
        mLastOutputReportWork = NULL;
    }
    if (mStopDoneEvent != nullptr)
        mStopDoneEvent = nullptr;

    if (mSecureMode)
        sConcurrentInstanceSecures.fetch_sub(1, std::memory_order_relaxed);
    else
        sConcurrentInstances.fetch_sub(1, std::memory_order_relaxed);

    if (mIsMaxResolution)
        sConcurrentMaxResolutionInstance.fetch_sub(1, std::memory_order_relaxed);
    if (mIntfImpl->getInputCodec() == InputCodec::VC1) {
        sConcurrentVc1Instance.fetch_sub(1, std::memory_order_relaxed);
    }

    updateComponentState(ComponentState::DESTROYED);
    done->Signal();
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] done", __func__);
}

void C2VdecComponent::onStart(media::VideoCodecProfile profile, ::base::WaitableEvent* done) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    updateComponentState(ComponentState::STARTING);
    C2Vdec_LOG(CODEC2_LOG_INFO, "OnStart DolbyVision:%d", mIsDolbyVision);
    //CHECK_EQ(mComponentState, ComponentState::UNINITIALIZED);
    bool disableRC = property_get_bool(C2_PROPERTY_VDEC_DISABLE_RC, true);
    if (!disableRC) {
        struct sched_param param = {0};
        param.sched_priority = 1;
        if (sched_setscheduler(0, SCHED_RR, &param) != 0) {
            C2Vdec_LOG(CODEC2_LOG_ERR, "set sched_priority error: %s", strerror(errno));
        }
    } else {
        int niceval = -10;
        int priorityval = 0;
        propGetInt(C2_PROPERTY_VDEC_DEBUG_PRIORITY, &priorityval);
        niceval = ((priorityval > 0) ? (priorityval - 120) : (-10));
        if (setpriority(PRIO_PROCESS, 0, niceval) != 0) {
            C2Vdec_LOG(CODEC2_LOG_ERR, "setpriority error: %s, niceval:%d", strerror(errno), niceval);
        }
    }
    if (mDebugUtil) {
        mDebugUtil->setComponent(shared_from_this());
        mDebugUtil->start();
    }
    if (!isTunnerPassthroughMode()) {
        if (mVideoDecWraper) {
            mVideoDecWraper.reset();
            mVideoDecWraper = NULL;
        }
        mVideoDecWraper = std::make_shared<VideoDecWraper>();

        if (mDeviceUtil) {
            removeObserver(mDeviceUtil);
            mDeviceUtil.reset();
            mDeviceUtil = NULL;
        }
        mDeviceUtil = std::make_shared<DeviceUtil>(mSecureMode);
        addObserver(mDeviceUtil, static_cast<int>(mComponentState), mCompHasError);
        mDeviceUtil->setComponent(shared_from_this());
        mDeviceUtil->setHDRStaticColorAspects(GetIntfImpl()->getColorAspects());
        // set session id
        mPlayerId = mDeviceUtil->getPlayerId();
        mSessionID = mInstanceID;
        if (mPlayerId > 0) {
            mSessionID = mPlayerId;
        }
        mVideoDecWraper->setSessionID((uint32_t)mSessionID);
        mDeviceUtil->codecConfig(&mConfigParam);

        //update profile for DolbyVision
        if (mIsDolbyVision) {
            media::VideoDecodeAccelerator::SupportedProfiles supportedProfiles;
            InputCodec codec = mIntfImpl->getInputCodec();
            if (InputCodec::H264 <= codec || codec < InputCodec::UNKNOWN) {
                supportedProfiles = VideoDecWraper::AmVideoDec_getSupportedProfiles((uint32_t)codec);
                if (supportedProfiles.empty()) {
                    C2Vdec_LOG(CODEC2_LOG_ERR, "No supported profile from input codec: %d", mIntfImpl->getInputCodec());
                    return;
                }
                mCodecProfile = supportedProfiles[0].profile;
                mIntfImpl->updateCodecProfile(mCodecProfile);
                C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Update profile(%d) to (%d) mime(%s)", profile, mCodecProfile,
                    VideoCodecProfileToMime(mCodecProfile));
                profile = mCodecProfile;
            }
        }
        uint32_t vdecflags = AM_VIDEO_DEC_INIT_FLAG_CODEC2;
        if (mIntfImpl->mVdecWorkMode->value == VDEC_STREAMMODE) {
            vdecflags |= AM_VIDEO_DEC_INIT_FLAG_STREAMMODE;
            mVideoDecWraper->setPipeLineWorkNumber(mIntfImpl->mStreamModePipeLineDelay->value + mIntfImpl->mActualOutputDelay->value);
        }
        if (mIntfImpl->mDataSourceType->value == DATASOURCE_DMX)
            vdecflags |= AM_VIDEO_DEC_INIT_FLAG_DMXDATA_SOURCE;
        if (mDeviceUtil->isLowLatencyMode()) {
            vdecflags |= AM_VIDEO_DEC_INIT_FLAG_USE_LOW_LATENCY_MODE;
        }

        char mInstanceName[32];
        snprintf(mInstanceName, sizeof(mInstanceName), "CODEC2-%d", mSessionID);
        mVdecInitResult = (VideoDecodeAcceleratorAdaptor::Result)mVideoDecWraper->initialize(VideoCodecProfileToMime(profile),
                (uint8_t*)&mConfigParam, sizeof(mConfigParam), mSecureMode, this, vdecflags, mInstanceName, QuitEventFunc, (void *)this);
        //set some decoder config
        //set unstable state and duration to vdec
        mDeviceUtil->setUnstable();
        mDeviceUtil->setDuration();
        mDecoderID = mVideoDecWraper->getDecoderID();
        TraceInit();
        prctl(PR_SET_NAME, (unsigned long) TRACE_NAME_VDEC_COMPONENT_THREAD.str().c_str());
    } else {
        mVdecInitResult = VideoDecodeAcceleratorAdaptor::Result::SUCCESS;
    }

    if (mDequeueThreadUtil == nullptr) {
        mDequeueThreadUtil = std::make_shared<DequeueThreadUtil>();
    }
    mDequeueThreadUtil->setComponent(shared_from_this());

    if (isTunnelMode() && mTunnelHelper) {
        mTunnelHelper->start();
    }
    if (isTunnerPassthroughMode() && mTunerPassthroughHelper) {
        mTunerPassthroughHelper->start();
    }

    if (!mSecureMode && (mIntfImpl->getInputCodec() == InputCodec::H264
                || mIntfImpl->getInputCodec() == InputCodec::H265
                || mIntfImpl->getInputCodec() == InputCodec::MP2V)) {
        // Get default color aspects on start.
        updateColorAspects();
    }

    if (mVdecInitResult == VideoDecodeAcceleratorAdaptor::Result::SUCCESS) {
        updateComponentState(ComponentState::STARTED, false);
    }

    done->Signal();
}


void C2VdecComponent::TraceInit() {
    TRACE_NAME_IN_PTS.str("");
    TRACE_NAME_BITSTREAM_ID.str("");
    TRACE_NAME_OUT_PTS.str("");
    TRACE_NAME_FETCH_OUT_BLOCK_ID.str("");
    TRACE_NAME_FINISHED_WORK_PTS.str("");
    TRACE_NAME_SEND_OUTPUT_BUFFER.str("");
    TRACE_NAME_VDEC_COMPONENT_THREAD.str("");

    TRACE_NAME_IN_PTS << mSessionID << "-" << mDecoderID << "-c2InPTS";
    TRACE_NAME_BITSTREAM_ID << mSessionID << "-" << mDecoderID << "-c2InBitStreamID";
    TRACE_NAME_OUT_PTS << mSessionID << "-" << mDecoderID << "-c2OutPts";
    TRACE_NAME_FETCH_OUT_BLOCK_ID << mSessionID << "-" << mDecoderID << "-c2FetchOutBlockId";
    TRACE_NAME_FINISHED_WORK_PTS << mSessionID << "-" << mDecoderID << "-c2FinishedWorkPTS";
    TRACE_NAME_SEND_OUTPUT_BUFFER << mSessionID << "-" << mDecoderID << "-c2SendOutPutBuffer";
    TRACE_NAME_VDEC_COMPONENT_THREAD << mSessionID << "-" << mDecoderID << "-C2VdecComponentThread";
}

void C2VdecComponent::onQueueWork(std::unique_ptr<C2Work> work, std::shared_ptr<C2StreamHdrDynamicMetadataInfo::input> info) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    CODEC2_ATRACE_CALL();
    if (mFlushDoneWithOutEosWork == true)
        onReusedOutBuf();

    uint32_t drainMode = NO_DRAIN;
    bool isEosWork = false;
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "Input EOS");
        drainMode = DRAIN_COMPONENT_WITH_EOS;
        isEosWork = true;
    }

    if ((work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) == 0 && (mFirstInputTimestamp == -1)) {
        mFirstInputTimestamp = work->input.ordinal.timestamp.peekull();
    }

    CODEC2_VDEC_ATRACE(TRACE_NAME_IN_PTS.str().c_str(), work->input.ordinal.timestamp.peekull());
    CODEC2_VDEC_ATRACE(TRACE_NAME_BITSTREAM_ID.str().c_str(), work->input.ordinal.frameIndex.peekull());

    if (mIntfImpl->mVendorGameModeLatency->enable) {
        uint64_t now = systemTime();
        if ((now - mLastInputTimestamp < 8000000) && (mInputWorkCount > 32)) {
            int32_t bitstreamId = frameIndexToBitstreamId(work->input.ordinal.frameIndex);
            mDropFrameForLatency.push(bitstreamId);
            CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"[%d] drop frame(%d/%zd) when frames too closed.", __LINE__, bitstreamId, mDropFrameForLatency.size());
        } else {
            mLastInputTimestamp = now;
        }
    }

    mInputWorkCount ++;
    mInputQueueNum ++;
    BufferStatus(this, CODEC2_LOG_TAG_BUFFER, "queue input index=%llu, timestamp=%llu, flags=0x%x",
            work->input.ordinal.frameIndex.peekull(), work->input.ordinal.timestamp.peekull(), work->input.flags);
    if (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) {
        mInputCSDWorkCount ++;
    }
    //std::shared_ptr<C2StreamHdr10PlusInfo::input> info((mIntfImpl->getHdr10PlusInfo()));
    mQueue.push({std::move(work), drainMode, info});

    //  TODO: set a maximum size of mQueue and check if mQueue is already full.
    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onDequeueWork, ::base::Unretained(this)));

    if (mReportEosWork == true || (mHaveFlushDone == true)) {
        mReportEosWork = false;
        mHaveDrainDone = false;
        mHaveFlushDone = false;
        reStartAllocTask();
        // mDequeueThreadUtil->StartAllocBuffer();
        // if (!mDequeueThreadUtil->StartRunDequeueTask(mOutputFormat.mCodedSize, static_cast<uint32_t>(mOutputFormat.mPixelFormat))) {
        //     C2Vdec_LOG(CODEC2_LOG_ERR, "[%s:%d] StartDequeueThread Failed", __func__, __LINE__);
        // }
    }
}

void C2VdecComponent::reStartAllocTask() {
    if ((mDequeueThreadUtil != nullptr) && isNonTunnelMode()) {
        int bufferInClient = mGraphicBlockStateCount[(int)GraphicBlockInfo::State::OWNED_BY_CLIENT];
        int bufferInAcc = mGraphicBlockStateCount[(int)GraphicBlockInfo::State::OWNED_BY_ACCELERATOR];
        int bufferInCom = mGraphicBlockStateCount[(int)GraphicBlockInfo::State::OWNED_BY_COMPONENT];
        //we need check stopped task count.we used total count - alloced buf count.
        //we start post task to alloc outbuf if stopCount and bufferInClient is not eq 0.
        int stopCount =   mOutputFormat.mMinNumBuffers - bufferInClient - bufferInAcc - bufferInCom;
        if (stopCount > 0)
            bufferInClient = bufferInClient + stopCount;
        //flush will clear dalay count info, we need update delay count when flush done.
        //so we add update delay count when we restart alloc task.
        updateOutputDelayBufCount();
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] stop[%d]all[%d]client[%d] hal[%d]com[%d]buffer in client and post dequeue task [%s][%d]", __func__,stopCount,mOutputFormat.mMinNumBuffers, bufferInClient,bufferInAcc, bufferInCom, mCurrentBlockSize.ToString().c_str() , mCurrentPixelFormat);
        uint32_t frameDur = mDeviceUtil->getVideoDurationUs();
        if (!mDequeueThreadUtil->StartRunDequeueTask(mOutputFormat.mCodedSize, static_cast<uint32_t>(mOutputFormat.mPixelFormat))) {
                C2Vdec_LOG(CODEC2_LOG_ERR, "[%s:%d] StartDequeueThread Failed", __func__, __LINE__);
        }
        mDequeueThreadUtil->StartAllocBuffer();
        if (mBufferFirstAllocated == true) {
            for (int i = 1; i <= bufferInClient; i++) {
                mDequeueThreadUtil->postDelayedAllocTask(mCurrentBlockSize, mCurrentPixelFormat, true, static_cast<uint32_t>(i * frameDur));
            }
        }
    }
}

void C2VdecComponent::onReusedOutBuf() {
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] into.", __func__);
    if (mFlushDoneWithOutEosWork == true && mBufferFirstAllocated == true) {
        mFlushDoneWithOutEosWork = false;
        while (!mFlushDoneBufferOwnedByComp.empty()) {
            auto ite = mFlushDoneBufferOwnedByComp.front();
            GraphicBlockInfo* info = getGraphicBlockById(ite);
            if (info == NULL) {
                mFlushDoneBufferOwnedByComp.pop_front();
                C2Vdec_LOG(CODEC2_LOG_ERR, "[%s] info is null, please check it.", __func__);
                continue;
            }

            if (info->mState == GraphicBlockInfo::State::OWNED_BY_COMPONENT) {
                GraphicBlockStateChange(this, info, GraphicBlockInfo::State::OWNED_BY_ACCELERATOR);
                BufferStatus(this, CODEC2_LOG_TAG_BUFFER, "[%s] index=%d", __func__, info->mBlockId);
            }
            mFlushDoneBufferOwnedByComp.pop_front();
        }

        //after flush we need reuse the buffer which owned by accelerator
        for (auto& info : mGraphicBlocks) {
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] Index:%d,graphic block status:%s count:%ld", __func__,
                    info.mBlockId, GraphicBlockState(info.mState), info.mGraphicBlock.use_count());
            if (info.mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR) {
                sendOutputBufferToAccelerator(&info, false);
            }
        }
    } else if (mFlushDoneWithOutEosWork == true) {
        //if putbuf is not allocated,we only send outbuf to decoder when
        //allocate buf, no need send outbuf again.
        mFlushDoneWithOutEosWork = false;
    }
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] end.", __func__);
}


void C2VdecComponent::onDequeueWork() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    uint32_t queueWorkCount = mQueue.size();
    RETURN_ON_UNINITIALIZED_OR_ERROR();
    if (mQueue.empty()) {
        return;
    }
    if (mComponentState == ComponentState::DRAINING ||
        mComponentState == ComponentState::FLUSHING) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "Temporarily stop dequeueing works since component is draining/flushing.");
        return;
    }
    if (mComponentState != ComponentState::STARTED) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Work queue should be empty if the component is not in STARTED state.");
        return;
    }

    if (mFlushDoneWithOutEosWork == true) {
        onReusedOutBuf();
    }

    if (mHaveFlushDone == true) {
        mHaveFlushDone = false;
        reStartAllocTask();
    }

    // Dequeue a work from mQueue.
    std::unique_ptr<C2Work> work(std::move(mQueue.front().mWork));
    auto drainMode = mQueue.front().mDrainMode;
    std::shared_ptr<C2StreamHdrDynamicMetadataInfo::input> hdrInfo = mQueue.front().mHdr10PlusInfo;
    mQueue.pop();

    //CHECK_LE(work->input.buffers.size(), 1u);
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
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Got a work with no input buffer! Emplace a nullptr inside.");
        work->input.buffers.emplace_back(nullptr);
    } else if (work->input.buffers.front() != nullptr) {
        // If input.buffers is not empty, the buffer should have meaningful content inside.
        C2ConstLinearBlock linearBlock = work->input.buffers.front()->data().linearBlocks().front();
        //CHECK_GT(linearBlock.size(), 0u);

        // Send input buffer to Vdec for decode.
        int64_t timestamp = work->input.ordinal.timestamp.peekull();
        //check hdr10 plus
        uint8_t *hdr10plusBuf = nullptr;
        uint32_t hdr10plusLen = 0;
        C2ReadView rView = mDefaultDummyReadView;
        bool isHdr10PlusInfoWithWork = false;

        for (const std::unique_ptr<C2Param> &param : work->input.configUpdate) {
            switch (param->coreIndex().coreIndex()) {
                case C2StreamHdr10PlusInfo::CORE_INDEX:
                    {
                        C2StreamHdr10PlusInfo::input *hdr10PlusInfo =
                        C2StreamHdr10PlusInfo::input::From(param.get());
                        if (hdr10PlusInfo != nullptr) {
                            std::vector<std::unique_ptr<C2SettingResult>> failures;
                            std::unique_ptr<C2Param> outParam = C2Param::CopyAsStream(*param.get(), true /* out put*/, param->stream());

                            isHdr10PlusInfoWithWork = true;
                            hdr10plusBuf = hdr10PlusInfo->m.value;
                            hdr10plusLen = hdr10PlusInfo->flexCount();
                            c2_status_t err = mIntfImpl->config({outParam.get()}, C2_MAY_BLOCK, &failures);
                            if (err == C2_OK) {
                                work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*outParam.get()));
                            } else {
                                C2Vdec_LOG(CODEC2_LOG_ERR, "Config update hdr10Plus size Failed.");
                            }
                        }
                    }
                    break;
                case C2StreamHdrDynamicMetadataInfo::CORE_INDEX:
                    C2Vdec_LOG(CODEC2_LOG_INFO, "Config Update hdrDynamicMetadateInfo");
                    break;
                default:
                    break;
            }
        }
        if (!isHdr10PlusInfoWithWork) {
            if (hdrInfo != nullptr) {
                hdr10plusBuf = hdrInfo->m.data;
                hdr10plusLen = hdrInfo->flexCount();
                std::unique_ptr<C2StreamHdrDynamicMetadataInfo::output> hdr10PlusInfo =
                    C2StreamHdrDynamicMetadataInfo::output::AllocUnique(hdr10plusLen);
                hdr10PlusInfo->m.type_ = hdrInfo->m.type_;
                memcpy(hdr10PlusInfo->m.data, hdr10plusBuf, hdr10plusLen);
                work->worklets.front()->output.configUpdate.push_back(std::move(hdr10PlusInfo));
            } else {
                mIntfImpl->updateHdr10PlusInfoToWork(*work);
                mIntfImpl->getHdr10PlusBuf(&hdr10plusBuf, &hdr10plusLen);
            }
        }

        if (gloglevel & CODEC2_LOG_DEBUG_LEVEL2 && (hdr10plusLen > 0)) {
            AString tmp;
            hexdump(hdr10plusBuf, hdr10plusLen, 4, &tmp);
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "update Container HDR10+ info ID:%d, timestamp:%lld size:%d, data:", bitstreamId, (long long)timestamp, hdr10plusLen);
            ALOGD("%s", tmp.c_str());
        }
        sendInputBufferToAccelerator(linearBlock, bitstreamId, timestamp, work->input.flags, (unsigned char *)hdr10plusBuf, hdr10plusLen);
    }

    //CHECK_EQ(work->worklets.size(), 1u);
    work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(0);
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->input.ordinal.customOrdinal = work->input.ordinal.timestamp.peekull();

    if (drainMode != NO_DRAIN) {
        if (mVideoDecWraper) {
            mVideoDecWraper->eosFlush();
        }
        updateComponentState(ComponentState::DRAINING);
        mPendingOutputEOS = drainMode == DRAIN_COMPONENT_WITH_EOS;
    }

    // Put work to mPendingWorks.
    CODEC2_LOG(CODEC2_LOG_TAG_BUFFER, "OnDequeueWork,queue work size:%d put pending work bitId:%d, pending work size:%zd",
            queueWorkCount, frameIndexToBitstreamId(work->input.ordinal.frameIndex.peeku()), mPendingWorks.size());

    mPendingWorks.emplace_back(std::move(work));

    if (isEmptyCSDWork || isEmptyWork) {
        // Directly report the empty CSD work as finished.
        C2Vdec_LOG(CODEC2_LOG_INFO, "OnDequeueWork empty csd work, bitId:%d\n", bitstreamId);
        reportWorkIfFinished(bitstreamId, 0, isEmptyWork);
    }
    if (!mQueue.empty()) {
        mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onDequeueWork,
                                                      ::base::Unretained(this)));
    }
}

void C2VdecComponent::onInputBufferDone(int32_t bitstreamId) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    C2Work* work = getPendingWorkByBitstreamId(bitstreamId);
    if (!work) {
        if (mIntfImpl->mVendorGameModeLatency->enable) {
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s:%d] bitstreamId:%d gamemode already workdone", __func__, __LINE__, bitstreamId);
            return;
        } else {
            C2Vdec_LOG(CODEC2_LOG_ERR, "[%s:%d] Can not get pending work with bitstreamId:%d", __func__, __LINE__,  bitstreamId);
            reportError(C2_CORRUPTED);
            return;
        }
    }

    // When the work is done, the input buffer shall be reset by component.
    work->input.buffers.front().reset();
    mInputQueueNum --;
    BufferStatus(this, CODEC2_LOG_TAG_BUFFER, "queue input done index=%d flag=%d pending size:%zu",
            bitstreamId, (int32_t)work->input.flags, mPendingBuffersToWork.size());

    //report csd work and drop work
    if ((work->input.flags & C2FrameData::FLAG_CODEC_CONFIG)
        || (work->worklets.front()->output.flags & C2FrameData::FLAG_DROP_FRAME)
        || (work->input.flags & C2FrameData::FLAG_DROP_FRAME)) {
        if (work->input.flags & C2FrameData::FLAG_DROP_FRAME) {
            c2_cntr64_t timestamp = work->worklets.front()->output.ordinal.timestamp + work->input.ordinal.customOrdinal.peekull()
                - work->input.ordinal.timestamp;
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "drop frame(bitstreamid %lld,pts:%lld) as app required",
                    (long long)bitstreamId, timestamp.peekull());
        }

        if (isNonTunnelMode()) {
            reportWorkIfFinished(bitstreamId, 0);
        } else if (mTunnelHelper && isTunnelMode()) {
            mTunnelHelper->fastHandleWorkTunnel(bitstreamId, 0);
        }
    } else {
       if (!mPendingBuffersToWork.empty() && isNonTunnelMode()) {
            mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::sendOutputBufferToWorkIfAnyTask,
                                             ::base::Unretained(this),
                                             false));
        }
    }
}

void C2VdecComponent::onOutputBufferReturned(std::shared_ptr<C2GraphicBlock> block,uint32_t poolId,
                                            uint32_t blockId) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    if ((block->width() != static_cast<uint32_t>(mOutputFormat.mCodedSize.width()) ||
        block->height() != static_cast<uint32_t>(mOutputFormat.mCodedSize.height())) &&
        (block->width() != mDeviceUtil->getOutAlignedSize(mOutputFormat.mCodedSize.width(), true) ||
        block->height() != mDeviceUtil->getOutAlignedSize(mOutputFormat.mCodedSize.height(), true))) {
        // Output buffer is returned after we changed output resolution. Just let the buffer be
        // released.
        C2Vdec_LOG(CODEC2_LOG_TAG_BUFFER, "Discard obsolete graphic block: poolId=%u", poolId);
        return;
    }

    //pending outbuffer
    if (mHaveDrainDone == true) {
        C2Vdec_LOG(CODEC2_LOG_TAG_BUFFER, "have receive eos, pending output buffer poolId=%u", poolId);
        return;
    }

    GraphicBlockInfo* info = getGraphicBlockByBlockId(poolId, blockId);
    if (!info) {
        //need to rebind poolid vs blockid
        info = getUnbindGraphicBlock();
        if (!info) {
            if (!mPendingGraphicBlockBuffer) {
                int blockUseCount = block.use_count();
                mPendingGraphicBlockBuffer = std::move(block);
                mPendingGraphicBlockBufferId = blockId;
                C2Vdec_LOG(CODEC2_LOG_TAG_BUFFER, "Now pending blockId: %d, count:%d", blockId, blockUseCount);
                return;
            }
            reportError(C2_CORRUPTED);
            return;
        }
        info->mPoolId = poolId;
    }

    if (!info->mBind) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "After realloc graphic, rebind %d->%d", poolId, info->mBlockId);
        info->mBind = true;
    }

    if (getVideoResolutionChanged() &&
        info->mState != GraphicBlockInfo::State::OWNED_BY_CLIENT) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Graphic block (id=%d) (state=%s) (fd=%d) (fdset=%d)should be owned by client on return",
                info->mBlockId, GraphicBlockState(info->mState), info->mFd,info->mFdHaveSet);
        if (mDebugUtil)
            mDebugUtil->showGraphicBlockInfo();
        reportError(C2_BAD_STATE);
        return;
    }
    info->mGraphicBlock = std::move(block);
    GraphicBlockStateChange(this, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
    CODEC2_VDEC_ATRACE(TRACE_NAME_FETCH_OUT_BLOCK_ID.str().c_str(), info->mBlockId);
    BufferStatus(this, CODEC2_LOG_TAG_BUFFER, "outbuf return index=%d", info->mBlockId);

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

void C2VdecComponent::onNewBlockBufferFetched(std::shared_ptr<C2GraphicBlock> block, uint32_t poolId,
                                            uint32_t blockId) {

    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();
    if (block == nullptr) {
        C2Vdec_LOG(CODEC2_LOG_TAG_BUFFER, "[%s] got null block, do not use it", __func__);
        return;
    }

    if (isResolutionChanging()) {
        C2Vdec_LOG(CODEC2_LOG_TAG_BUFFER, "[%s] resolutionchanging true, return", __func__);
        return;
    }
    if (mComponentState == ComponentState::STOPPING) {
        C2Vdec_LOG(CODEC2_LOG_TAG_BUFFER, "[%s:%d] mComponentState is STOPPING, return", __func__, __LINE__);
        return;
    }
    if (getVideoResolutionChanged()) {
        if ((mDeviceUtil->getOutAlignedSize(mOutputFormat.mCodedSize.width()) == block->width() &&
             mDeviceUtil->getOutAlignedSize(mOutputFormat.mCodedSize.height()) == block->height())) {
            appendOutputBuffer(std::move(block), poolId, blockId, true);
            GraphicBlockInfo *info = getGraphicBlockByBlockId(poolId, blockId);
            if (info == nullptr) {
                C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] got null blockinfo, donot use it", __func__);
                return;
            }
            GraphicBlockStateInit(this, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
            CODEC2_VDEC_ATRACE(TRACE_NAME_FETCH_OUT_BLOCK_ID.str().c_str(), info->mBlockId);
            BufferStatus(this, CODEC2_LOG_TAG_BUFFER, "fetch new outbuf index=%u", info->mBlockId);
            sendOutputBufferToAccelerator(info, true /*ownByAccelerator*/);
        } else {
            C2Vdec_LOG(CODEC2_LOG_TAG_BUFFER, "Fetch current block(%d*%d) is pending and reset it.", block->width(), block->height());
            block.reset();
        }
    } else {
        if ((mOutputFormat.mCodedSize.width() == block->width() &&
             mOutputFormat.mCodedSize.height() == block->height()) ||
            (mDeviceUtil->getOutAlignedSize(mOutputFormat.mCodedSize.width(), true) == block->width() &&
             mDeviceUtil->getOutAlignedSize(mOutputFormat.mCodedSize.height(), true) == block->height())){
            C2Vdec_LOG(CODEC2_LOG_TAG_BUFFER, "Current resolution (%d*%d) new block(%d*%d) and add it",
                mOutputFormat.mCodedSize.width(), mOutputFormat.mCodedSize.height(), block->width(), block->height());
            appendOutputBuffer(std::move(block), poolId, blockId, true);
            GraphicBlockInfo *info = getGraphicBlockByBlockId(poolId, blockId);
            if (info == nullptr) {
                C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] got null 1 blockinfo, donot use it", __func__);
                return;
            }
            GraphicBlockStateInit(this, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
            CODEC2_VDEC_ATRACE(TRACE_NAME_FETCH_OUT_BLOCK_ID.str().c_str(), info->mBlockId);
            BufferStatus(this, CODEC2_LOG_TAG_BUFFER, "fetch new outbuf index=%d", info->mBlockId);
            sendOutputBufferToAccelerator(info, true /*ownByAccelerator*/);
        } else {
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Fetch current block(%d*%d) is pending", block->width(), block->height());
        }
    }
}

bool C2VdecComponent::checkIsSentId(int64_t bitstreamId) {
    std::list<int64_t>::iterator iter;
    bool send = false;
    for (iter = mSentOutBitStreamIdList.begin(); iter != mSentOutBitStreamIdList.end(); iter++) {
        if (*iter == bitstreamId) {
            send = true;
            break;
        }
    }
    return send;
}

void C2VdecComponent::onOutputBufferDone(int32_t pictureBufferId, int64_t bitstreamId, int32_t flags, uint64_t timestamp) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();
    if (mResChStat == C2_RESOLUTION_CHANGING) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "onOutputBufferDone :%d mResChStat C2_RESOLUTION_CHANGING,ignore this buf", pictureBufferId);
    }

    if (mComponentState == ComponentState::FLUSHING) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s] in flushing, pending bitstreamId=%" PRId64 " first", __func__, bitstreamId);
        mLastOutputBitstreamId = bitstreamId;
        return;
    }

    GraphicBlockInfo* info = getGraphicBlockById(pictureBufferId);

    if (info == NULL) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Can't get graphic block pictureBufferId:%d", pictureBufferId);
        reportError(C2_CORRUPTED);
        return;
    }

    if (info->mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR) {
        GraphicBlockStateChange(this, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
    }

    C2Work* work = NULL;
    if (mLastOutputBitstreamId != bitstreamId) {
        work = getPendingWorkByBitstreamId(bitstreamId);
        if (!work && checkIsSentId(bitstreamId) == false) {
            CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1,"[%d] Can't found bitstreamId at pending and sent list,should have been flushed or dropped:%" PRId64 "", __LINE__,bitstreamId);
            sendOutputBufferToAccelerator(info, true);
            return;
        }

        if (work != NULL && !mDropFrameForLatency.empty() && (mDropFrameForLatency.front() == bitstreamId)) {
            CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2,"[%d]gamemode frame dropped:%" PRId64 "", __LINE__, bitstreamId);
            mDropFrameForLatency.pop();
            work->worklets.front()->output.flags = C2FrameData::FLAG_DROP_FRAME;
            reportWorkIfFinished(bitstreamId, 0);
            sendOutputBufferToAccelerator(info, true);
            return;
        }
    }
    //work is nullptr or this work input flag is not drop frame
    if (((work == NULL)
            || ((work->input.flags & C2FrameData::FLAG_DROP_FRAME) == 0))) {
        mSentOutBitStreamIdList.push_front(bitstreamId);
    }
    if (mSentOutBitStreamIdList.size() > 20) {
        mSentOutBitStreamIdList.pop_back();
    }
    if (mDebugUtil) {
        mDebugUtil->fillBufferDone(info, flags, mOutputWorkCount, timestamp, bitstreamId, pictureBufferId);
    }
    mPendingBuffersToWork.push_back({(int32_t)bitstreamId, pictureBufferId, timestamp, flags, false});
    mOutputWorkCount ++;
    CODEC2_VDEC_ATRACE(TRACE_NAME_OUT_PTS.str().c_str(), timestamp);
    BufferStatus(this, CODEC2_LOG_TAG_BUFFER, "outbuf from videodec index=%" PRId64 ", pictureid=%d, fags:%d pending size=%zu",
            bitstreamId, pictureBufferId, flags, mPendingBuffersToWork.size());

    mLastOutputBitstreamId = bitstreamId;
    if (isNonTunnelMode()) {
        sendOutputBufferToWorkIfAny(false /* dropIfUnavailable */);
    } else if (isTunnelMode() && mTunnelHelper != NULL) {

        if ((flags & (int)PictureFlag::PICTURE_FLAG_ERROR_FRAME) != 0) {
           mTunnelHelper->fastHandleOutBufferTunnel(timestamp, pictureBufferId);
           return;
        }
        if (mHDR10PlusMeteDataNeedCheck) {
            unsigned char  buffer[META_DATA_SIZE];
            int buffer_size = 0;
            memset(buffer, 0, META_DATA_SIZE);
            mDeviceUtil->getUvmMetaData(info->mFd, buffer, &buffer_size);
            bool gotDur = false;
            if (buffer_size > META_DATA_SIZE) {
                C2Vdec_LOG(CODEC2_LOG_ERR, "Uvm metadata size error, please check");
            } else if (buffer_size <= 0)  {
                //Do not have meta data, do not need check more.
                mHDR10PlusMeteDataNeedCheck = false;
            } else {
                gotDur = mDeviceUtil->parseAndProcessDuration(buffer, buffer_size);
                if (gotDur == true) {
                    C2Vdec_LOG(CODEC2_LOG_INFO, "Got decoder duration");
                    mHDR10PlusMeteDataNeedCheck = false;
                }
            }
            mUpdateDurationUsCount++;
        }
        if ((work != NULL)
            && ((work->input.flags & C2FrameData::FLAG_DROP_FRAME) != 0)) {
             mTunnelHelper->fastHandleOutBufferTunnel(timestamp, pictureBufferId);
             return;
       }

       mTunnelHelper->sendVideoFrameToVideoTunnel(pictureBufferId, bitstreamId,timestamp);
       return;
    }

    if (mOutputWorkCount == 1 && (mDequeueThreadUtil->getAllocBufferLoopState() == false)) {
        mDequeueThreadUtil->StartAllocBuffer();
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Enable queuethread Running...");
    }

    mDequeueThreadUtil->postDelayedAllocTask(mCurrentBlockSize, mCurrentPixelFormat, true, mDeviceUtil->getVideoDurationUs());
}

C2Work* C2VdecComponent::cloneWork(C2Work* ori) {
    C2Work* n = new C2Work();
    if (n == NULL) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Clone Work, malloc memory Failed.");
        return NULL;
    }

    if (ori == NULL) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Origin work is null, clone work Failed.");
        if (n != NULL) {
            delete n;
            n = NULL;
        }
        return NULL;
    }

    n->input.flags = ori->input.flags;
    n->input.ordinal = ori->input.ordinal;
    n->worklets.emplace_back(new C2Worklet);
    n->worklets.front()->output.ordinal = n->input.ordinal;

    return n;
}

void C2VdecComponent::sendClonedWork(C2Work* work, int32_t flags) {
    work->worklets.front()->output.flags = C2FrameData::FLAG_INCOMPLETE;
    work->result = C2_OK;
    work->workletsProcessed = 1;

    //save last out pts
    mDeviceUtil->setLastOutputPts(work->input.ordinal.customOrdinal.peekull());
    //work->input.ordinal.customOrdinal = mDeviceUtil->checkAndAdjustOutPts(work,flags);
    c2_cntr64_t timestamp = work->worklets.front()->output.ordinal.timestamp + work->input.ordinal.customOrdinal
                            - work->input.ordinal.timestamp;
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2,"Reported finished work index=%llu pts=%llu,%d", work->input.ordinal.frameIndex.peekull(), timestamp.peekull(),__LINE__);
    std::list<std::unique_ptr<C2Work>> finishedWorks;
    finishedWorks.emplace_back(std::move(std::unique_ptr<C2Work>(work)));
    mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
}

c2_status_t C2VdecComponent::sendOutputBufferToWorkIfAny(bool dropIfUnavailable) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());

    while (!mPendingBuffersToWork.empty()) {
        auto nextBuffer = mPendingBuffersToWork.front();
        GraphicBlockInfo* info = getGraphicBlockById(nextBuffer.mBlockId);

        if (info == NULL) {
            C2Vdec_LOG(CODEC2_LOG_ERR, "[%s]Graphic Block Info null", __func__);
            return C2_BAD_STATE;
        }

        CODEC2_LOG(CODEC2_LOG_TAG_BUFFER,"[%s] Get pending bitstream id:%d, block id:%d inode:%" PRId64 " size:%zd",
            __func__, nextBuffer.mBitstreamId, nextBuffer.mBlockId,
            mBlockPoolUtil->getBlockInodeByBlockId(nextBuffer.mBlockId),
            mPendingBuffersToWork.size());
        bool isSendCloneWork = false;
        if (info->mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR) {
            C2Vdec_LOG(CODEC2_LOG_ERR, "Graphic block (id=%d) should not be owned by accelerator", info->mBlockId);
            reportError(C2_BAD_STATE);
            return C2_BAD_STATE;
        }

        C2Work* work = NULL;
        //for one packet contains more than one frame.
        work = getPendingWorkByBitstreamId(nextBuffer.mBitstreamId);
        if (!work) {
            isSendCloneWork = true;
            work = cloneWork(mLastOutputReportWork);
        } else {
            if (isInputWorkDone(work) == false) {
                if (mIntfImpl->mVendorGameModeLatency->enable) {
                    work->input.buffers.front().reset();
                    mInputQueueNum --;
                } else {
                     C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s] input work done error. size(%zd)", __func__, mPendingBuffersToWork.size());
                    return C2_OK;
                }
            }
        }

        if (!work) {
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Discard bitstreamid after flush or reset :%d", __FUNCTION__, __LINE__, nextBuffer.mBitstreamId);
            GraphicBlockStateChange(this, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
            BufferStatus(this, CODEC2_LOG_TAG_BUFFER, "discard index=%d", info->mBlockId);
            sendOutputBufferToAccelerator(info, true /* ownByAccelerator */);
            mPendingBuffersToWork.pop_front();
            return C2_OK;
        }
        if (isSendCloneWork == false && nextBuffer.mSetOutInfo == true) {
           c2_status_t status = reportWorkIfFinished(nextBuffer.mBitstreamId, nextBuffer.flags);
           if (status != C2_OK) {
                C2Vdec_LOG(CODEC2_LOG_ERR, "[%s] reportWorkIfFinished- error.size(%zd) workdone[%d]input[%d]", __func__, mPendingBuffersToWork.size(), isWorkDone(work),isInputWorkDone(work));
                return C2_OK;
           } else {
                mPendingBuffersToWork.pop_front();
                continue;
           }
        }
        if (isSendCloneWork == false) {
            mLastFinishedBitstreamId = nextBuffer.mBitstreamId;
            if (mLastOutputReportWork != NULL) {
                delete mLastOutputReportWork;
                mLastOutputReportWork = NULL;
            }
        }
        work->input.ordinal.customOrdinal = nextBuffer.mMediaTimeUs;
        if (mHDR10PlusMeteDataNeedCheck) {
            unsigned char buffer[META_DATA_SIZE];
            int bufferSize = 0;
            memset(buffer, 0, META_DATA_SIZE);
            mDeviceUtil->getUvmMetaData(info->mFd, buffer, &bufferSize);
            if (bufferSize > META_DATA_SIZE) {
                C2Vdec_LOG(CODEC2_LOG_ERR, "Uvm metadata size error, please check");
            } else if (bufferSize <= 0)  {
                //Do not have meta data, do not need check more.
                mHDR10PlusMeteDataNeedCheck = false;
            } else {
                mDeviceUtil->parseAndProcessMetaData(buffer, bufferSize, *work);
            }
            mUpdateDurationUsCount++;
        }
        if (mLastOutputReportWork == NULL)
            mLastOutputReportWork = cloneWork(work);
        if (mLastOutputReportWork == NULL) {
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Last work is null, malloc memory Failed.");
            return C2_BAD_VALUE;
        }

        if (info->mState == GraphicBlockInfo::State::OWNED_BY_CLIENT) {
            // This buffer is the existing frame and still owned by client.
            if (!dropIfUnavailable &&
                std::find(mUndequeuedBlockIds.begin(), mUndequeuedBlockIds.end(),
                    nextBuffer.mBlockId) == mUndequeuedBlockIds.end()) {
                C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Still waiting for existing frame returned from client...");
                return C2_TIMED_OUT;
            }
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Drop this frame...");
            sendOutputBufferToAccelerator(info, true /* ownByAccelerator */);
            work->worklets.front()->output.flags = C2FrameData::FLAG_DROP_FRAME;
        } else if ( ((int)nextBuffer.flags & (int)PictureFlag::PICTURE_FLAG_ERROR_FRAME) != 0) {
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Drop error frame :%d", __FUNCTION__, __LINE__, nextBuffer.mBitstreamId);
            sendOutputBufferToAccelerator(info, true /* ownByAccelerator */);
            work->worklets.front()->output.flags = C2FrameData::FLAG_DROP_FRAME;
        } else {
            // This buffer is ready to push into the corresponding work.
            // Output buffer will be passed to client soon along with mListener->onWorkDone_nb().
            GraphicBlockStateChange(this, info, GraphicBlockInfo::State::OWNED_BY_CLIENT);
            BufferStatus(this, CODEC2_LOG_TAG_BUFFER, "report to ccodec index=%d, bitstreamid=%d, pts=%llu",
                    info->mBlockId, nextBuffer.mBitstreamId, work->input.ordinal.timestamp.peekull());
            updateUndequeuedBlockIds(info->mBlockId);
            // Attach output buffer to the work corresponded to bitstreamId.

            updateWorkParam(work, info);
            info->mGraphicBlock.reset();
        }

        // Check no-show frame by timestamps for VP8/VP9 cases before reporting the current work.
        if (mIntfImpl->getInputCodec() == InputCodec::VP9) {
            detectNoShowFrameWorksAndReportIfFinished(work->input.ordinal);
        }

        if (isSendCloneWork) {
            sendClonedWork(work, nextBuffer.flags);
        } else {
            mPendingBuffersToWork.at(0).mSetOutInfo = true;
            c2_status_t status = reportWorkIfFinished(nextBuffer.mBitstreamId, nextBuffer.flags);
            if (status != C2_OK) {
                C2Vdec_LOG(CODEC2_LOG_ERR, "[%s] reportWorkIfFinished- error.size(%zd) workdone[%d]input[%d]", __func__, mPendingBuffersToWork.size(), isWorkDone(work),isInputWorkDone(work));
                return C2_OK;
            }
        }

        mPendingBuffersToWork.pop_front();
    }
    return C2_OK;
}

void C2VdecComponent::sendOutputBufferToWorkIfAnyTask(bool dropIfUnavailable) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    sendOutputBufferToWorkIfAny(dropIfUnavailable);
}

void C2VdecComponent::updateWorkParam(C2Work* work, GraphicBlockInfo* info) {
    if (work == nullptr || info == nullptr || info->mGraphicBlock == nullptr) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "[%s] Graphicblock or work is null and return.", __func__);
        return;
    }

    C2ConstGraphicBlock constBlock = info->mGraphicBlock->share(
                                    C2Rect(mOutputFormat.mVisibleRect.width(),
                                    mOutputFormat.mVisibleRect.height()),
                                    C2Fence());

    std::shared_ptr<C2Buffer> buffer = C2Buffer::CreateGraphicBuffer(std::move(constBlock));

    if (mDeviceUtil->isColorAspectsChanged()) {
        updateColorAspects();
    }
    if (mCurrentColorAspects) {
        buffer->setInfo(mCurrentColorAspects);
    }
    /* update hdr static info */
    if (mDeviceUtil->isHDRStaticInfoUpdated()) {
        updateHDRStaticInfo();
    }
    if (mCurrentHdrStaticInfo) {
        buffer->setInfo(mCurrentHdrStaticInfo);
    }

    /* update hdr10 plus info */
    if (mDeviceUtil->isHDR10PlusStaticInfoUpdated()) {
        updateHDR10PlusInfo();
    }

    if (mPictureSizeChanged && mCurrentSize != nullptr) {
        mPictureSizeChanged = false;
        work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*mCurrentSize));
        ALOGI("video size changed");
    }
    /*mOutputDelay update at tryChangeOutputFormat, this is no used*/
    //if (mOutputDelay != nullptr) {
    //    work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*mOutputDelay));
    //    mOutputDelay = nullptr;
    //}
    if (buffer == nullptr)
        C2Vdec_LOG(CODEC2_LOG_ERR, "[%s] buffer is null", __func__);

    work->worklets.front()->output.buffers.emplace_back(std::move(buffer));
}

void C2VdecComponent::onAndroidVideoPeek() {
    if (mTunnelHelper) {
        mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VdecComponent::TunnelHelper::onAndroidVideoPeek, ::base::Unretained(&(*mTunnelHelper))));
    }
}

void C2VdecComponent::updateUndequeuedBlockIds(int32_t blockId) {
    // The size of |mUndequeuedBlockIds| will always be the minimum buffer count for display.
    mUndequeuedBlockIds.push_back(blockId);
    mUndequeuedBlockIds.pop_front();
}

void C2VdecComponent::checkPreempting() {
    if (mComponentState >= ComponentState::STARTED && mComponentState < ComponentState::DESTROYING
        && Preempting()) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] cancel", __func__);
        reportError(C2_CANCELED);
    } else {
        mTaskRunner->PostDelayedTask(FROM_HERE,
            ::base::Bind(&C2VdecComponent::checkPreempting, ::base::Unretained(this)),
            ::base::TimeDelta::FromMilliseconds(100));
    }
}

void C2VdecComponent::onDrain(uint32_t drainMode) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "onDrain: mode = %u", drainMode);
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    if (!mQueue.empty()) {
        // Mark last queued work as "drain-till-here" by setting drainMode. Do not change drainMode
        // if last work already has one.
        if (mQueue.back().mDrainMode == NO_DRAIN) {
            mQueue.back().mDrainMode = drainMode;
        }
    } else if (!mPendingWorks.empty()) {
        // Neglect drain request if component is not in STARTED mode. Otherwise, enters DRAINING
        // mode and signal Vdec flush immediately.
        if (mComponentState == ComponentState::STARTED) {
            if (mVideoDecWraper) {
                mVideoDecWraper->eosFlush();
            }
            updateComponentState(ComponentState::DRAINING);
            mPendingOutputEOS = drainMode == DRAIN_COMPONENT_WITH_EOS;

            if (mTunnelHelper && isTunnelMode()) {
                mTunnelHelper->flush();
            }
            if (mTunerPassthroughHelper && isTunnerPassthroughMode()) {
                mTunerPassthroughHelper->flush();
            }
        } else {
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Neglect drain. Component in state: %d", mComponentState);
        }
    } else {
        // Do nothing.
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "No buffers in Vdec, drain takes no effect.");
    }
}

void C2VdecComponent::onDrainDone() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s]", __func__);
    if (mComponentState == ComponentState::DRAINING) {
        updateComponentState(ComponentState::STARTED);
    } else if (mComponentState == ComponentState::STOPPING) {
        if (mPendingOutputEOS) {
            C2Vdec_LOG(CODEC2_LOG_INFO, "[%s@%d]Return EOS work.", __func__, __LINE__);
            if (reportEOSWork() != C2_OK) {
                return;
            }
        }
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "The client signals stop right before Vdec notifies drain done. Let stop process goes.");
        return;
    } else if (mComponentState != ComponentState::FLUSHING) {
        // It is reasonable to get onDrainDone in FLUSHING, which means flush is already signaled
        // and component should still expect onFlushDone callback from Vdec.
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Unexpected state while onDrainDone(). State=%d", mComponentState);
        reportError(C2_BAD_STATE);
        return;
    }

    if (isTunnelMode()) {
        CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d] tunnel mode reset done", __FUNCTION__, __LINE__);
        mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VdecComponent::onDequeueWork, ::base::Unretained(this)));
        return;
    }

    // Drop all pending existing frames and return all finished works before drain done.
    if (sendOutputBufferToWorkIfAny(true /* dropIfUnavailable */) != C2_OK) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "SendOutputBufferToWorkIfAny Failed.") ;
        return;
    }

    if (mPendingOutputEOS) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "[%s@%d]Return EOS work.", __func__, __LINE__);
        if (reportEOSWork() != C2_OK) {
            return;
        }
    }

    // Work dequeueing was stopped while component draining. Restart it.
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VdecComponent::onDequeueWork, ::base::Unretained(this)));
}

void C2VdecComponent::onFlush() {
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s]", __func__);
    if (mComponentState == ComponentState::FLUSHING ||
        mComponentState == ComponentState::STOPPING) {
        return;
        // Ignore other flush request when component is flushing or stopping.
    }

    mLastFlushTimeMs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000000;
    mFirstInputTimestamp = -1;
    mLastOutputBitstreamId = -1;
    mLastFinishedBitstreamId = -1;
    updateComponentState(ComponentState::FLUSHING);
    if (mVideoDecWraper) {
        mVideoDecWraper->flush();
    }

    if (mTunnelHelper && isTunnelMode()) {
        mTunnelHelper->flush();
    }
    if (mTunerPassthroughHelper && isTunnerPassthroughMode()) {
        mTunerPassthroughHelper->flush();
    }

    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] Done", __func__);;
}

void C2VdecComponent::onStop(::base::WaitableEvent* done) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s]", __func__);
    if (mComponentState == ComponentState::UNINITIALIZED) {
        return;
    }
    // Stop call should be processed even if component is in error state.

    // Pop all works in mQueue and put into mAbandonedWorks.
    while (!mQueue.empty()) {
        mAbandonedWorks.emplace_back(std::move(mQueue.front().mWork));
        mQueue.pop();
    }
    mInputWorkCount = 0;
    mInputCSDWorkCount = 0;
    mStopDoneEvent = done;  // restore done event which should be signaled in onStopDone().
    updateComponentState(ComponentState::STOPPING);
    mDequeueThreadUtil->StopRunDequeueTask();
    if (mTunerPassthroughHelper) {
        mTunerPassthroughHelper->stop();
    }
    // Immediately release Vdec by calling onStopDone() if component is in error state. Otherwise,
    // send reset request to Vdec and wait for callback to stop the component gracefully.
    if (mCompHasError) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Component is in error state. Immediately call onStopDone().");
        onStopDone();
    } else if (mComponentState != ComponentState::FLUSHING) {
        // Do not request Vdec reset again before the previous one is done. If reset is already sent
        // by onFlush(), just regard the following NotifyResetDone callback as for stopping.
        uint32_t flags = 0;
        if (mVideoDecWraper) {
            mVideoDecWraper->stop(flags|RESET_FLAG_NOWAIT);
        }
    }
    if (mDebugUtil)
        mDebugUtil->stop();
}

void C2VdecComponent::resetInputAndOutputBufInfo() {
    mInputWorkCount = 0;
    mInputCSDWorkCount = 0;
    mOutputWorkCount = 0;
    mErrorFrameWorkCount = 0;
    mHasQueuedWork = false;
    mUpdateDurationUsCount = 0;
    mOutputFinishedWorkCount = 0;
    mInputQueueNum = 0;
}

void C2VdecComponent::onFlushOrStopDone() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s]", __func__);
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
        C2Vdec_LOG(CODEC2_LOG_ERR, "[%s:%d]", __FUNCTION__, __LINE__);
        reportError(C2_CORRUPTED);
    }
}

void C2VdecComponent::onFlushDone() {
    RETURN_ON_UNINITIALIZED_OR_ERROR();
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s]", __func__);

    mDequeueThreadUtil->StopRunDequeueTask();

    {
        AutoMutex l(mFlushDoneWorkLock);
        mFlushPendingWorkList.clear();
        // Pop all works in mQueue and put into mAbandonedWorks.
        while (!mQueue.empty()) {
            mAbandonedWorks.emplace_back(std::move(mQueue.front().mWork));
            mQueue.pop();
        }

        CODEC2_LOG(CODEC2_LOG_INFO, "[%s@%d] PendingWorks:%zd Queue:%zd AbandonedWorks:%zd PendingBuffersToWork:%zd",
                __func__, __LINE__,
                mPendingWorks.size(), mQueue.size(),
                mAbandonedWorks.size(), mPendingBuffersToWork.size());

        // Pop all works in mAbandonedWorks and put into flushedWork.
        for (auto& work : mAbandonedWorks) {
            // TODO: correlate the definition of flushed work result to framework.
            work->result = C2_NOT_FOUND;
            // When the work is abandoned, buffer in input.buffers shall reset by component.
            if (!work->input.buffers.empty()) {
                work->input.buffers.front().reset();
            }

            CODEC2_LOG(CODEC2_LOG_INFO, "[%s] %s mode abandon bitstreamid:%d mediatimeus:%llu", __func__,
                (isTunnelMode() ? "tunnel" : "no-tunnel"),
                frameIndexToBitstreamId(work->input.ordinal.frameIndex),
                work->input.ordinal.timestamp.peekull());

            mFlushPendingWorkList.emplace_back(std::move(work));
        }
        mAbandonedWorks.clear();

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
            if (!mUseBufferQueue && mFlushPendingWorkList.empty() && mIsReportEosWork && isNonTunnelMode()) {
                for (auto & info : mGraphicBlocks) {
                    if (info.mState == GraphicBlockInfo::State::OWNED_BY_COMPONENT) {
                        C2ConstGraphicBlock constBlock = info.mGraphicBlock->share(
                                    C2Rect(mOutputFormat.mVisibleRect.width(),
                                    mOutputFormat.mVisibleRect.height()),
                                    C2Fence());
                        std::shared_ptr<C2Buffer> buffer = C2Buffer::CreateGraphicBuffer(std::move(constBlock));
                        work->worklets.front()->output.buffers.emplace_back(std::move(buffer));
                        C2Vdec_LOG(CODEC2_LOG_INFO, "report abandoned work and add block.");
                    }
                }
                mIsReportEosWork = false;
            }
            CODEC2_LOG(CODEC2_LOG_INFO, "[%s] %s mode abandon bitstreamid:%d mediatimeus:%llu", __func__,
                (isTunnelMode() ? "tunnel" : "no-tunnel"),
                frameIndexToBitstreamId(work->input.ordinal.frameIndex),
                work->input.ordinal.timestamp.peekull());

                mFlushPendingWorkList.emplace_back(std::move(work));
        }

        mPendingWorks.clear();
        mFlushDoneBufferOwnedByComp.clear();

        while (!mPendingBuffersToWork.empty()) {
            auto nextBuffer = mPendingBuffersToWork.front();
            GraphicBlockInfo* info = getGraphicBlockById(nextBuffer.mBlockId);
            if (info == NULL) {
                C2Vdec_LOG(CODEC2_LOG_ERR, "[%s] info is null, please check it.", __func__);
                continue;
            }
            BufferStatus(this, CODEC2_LOG_TAG_BUFFER, "[%s] add flush done add index=%d", __func__,nextBuffer.mBlockId);
            mFlushDoneBufferOwnedByComp.push_back(nextBuffer.mBlockId);
            mPendingBuffersToWork.pop_front();
        }
        mPendingBuffersToWork.clear();

        if (mDeviceUtil != nullptr) {
            mDeviceUtil->flush();
            mDeviceUtil->setUnstable();
            mDeviceUtil->setDuration();
        }
        mSentOutBitStreamIdList.clear();
    }

    updateComponentState(ComponentState::STARTED);
    AutoMutex l(mFlushDoneLock);
    mFlushDoneCond.signal();
}

void C2VdecComponent::onStopDone() {
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s]", __func__);
    CHECK(mStopDoneEvent);

    if (mTunnelHelper) {
        mTunnelHelper->stop();
    }

    if (mDequeueThreadUtil) {
        mDequeueThreadUtil->StopRunDequeueTask();
    }
    //  TODO: At this moment, there may be C2Buffer still owned by client, do we need to
    // do something for them?
    reportAbandonedWorks();
    mPendingOutputFormat.reset();
    mPendingBuffersToWork.clear();
    mNoOutFrameWorkQueue.clear();
    mSentOutBitStreamIdList.clear();
    mHasQueuedWork = false;


    if (mTunerPassthroughHelper) {
        mTunerPassthroughHelper->stop();
    }

    if (mFdInfoDebugEnable && mDebugUtil) {
        mDebugUtil->showCurrentProcessFdInfo();
    }

    if (mDebugUtil) {
        mDebugUtil->showGraphicBlockInfo();
    }

    if (mDeviceUtil) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] clear decoder duration", __func__);
        mDeviceUtil->clearDecoderDuration();
    }
    if (mPendingGraphicBlockBuffer) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Clear pending blockId: %d, count:%ld",
                mPendingGraphicBlockBufferId, mPendingGraphicBlockBuffer.use_count());
        mPendingGraphicBlockBufferId = -1;
        mPendingGraphicBlockBuffer.reset();
    }
    mBufferFirstAllocated = false;
    mSurfaceUsageGot = false;
    for (auto& info : mGraphicBlocks) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "GraphicBlock reset, block Info Id:%d Fd:%d poolId:%d State:%s block use count:%ld",
            info.mBlockId, info.mFd, info.mPoolId, GraphicBlockState(info.mState), info.mGraphicBlock.use_count());
            info.mGraphicBlock.reset();
    }
    mGraphicBlocks.clear();
    if (mBlockPoolUtil != NULL) {
        mBlockPoolUtil->cancelAllGraphicBlock();
        mBlockPoolUtil.reset();
        mBlockPoolUtil = NULL;
    }

    updateComponentState(ComponentState::UNINITIALIZED);
    if (mStopDoneEvent != nullptr)
        mStopDoneEvent->Signal();

    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "OnStopDone OK");
}

c2_status_t C2VdecComponent::setListener_vb(const std::shared_ptr<C2Component::Listener>& listener,
        c2_blocking_t mayBlock) {
    UNUSED(mayBlock);
    //  TODO: API says this method must be supported in all states, however I'm quite not
    //                 sure what is the use case.
    if (mState.load() != State::LOADED) {
        return C2_BAD_STATE;
    }
    mListener = listener;
    return C2_OK;
}

void C2VdecComponent::sendInputBufferToAccelerator(const C2ConstLinearBlock& input,
        int32_t bitstreamId, uint64_t timestamp,int32_t flags,uint8_t *hdrbuf,uint32_t hdrlen) {
    //UNUSED(flags);
    int dupFd = dup(input.handle()->data[0]);
    if (dupFd < 0) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Failed to dup(%d) input buffer (bitstreamId:%d), errno:%d", input.handle()->data[0],
                bitstreamId, errno);
        reportError(C2_CORRUPTED);
        return;
    }
    CODEC2_LOG(CODEC2_LOG_TAG_BUFFER, "[%s@%d]Decode bitstream ID: %d timestamp:%" PRId64 " offset: %u size: %d hdrlen:%d flags 0x%x", __FUNCTION__,__LINE__,
            bitstreamId, timestamp, input.offset(), (int)input.size(), hdrlen, flags);
    if (mDebugUtil) {
        mDebugUtil->emptyBuffer((void*) input.handle(), timestamp, flags, input.size());
    }
    if (mVideoDecWraper != NULL) {
        mVideoDecWraper->decode(bitstreamId, dupFd, input.offset(), input.size(), timestamp, hdrbuf, hdrlen, flags);
        if (mIntfImpl->mVdecWorkMode->value == VDEC_STREAMMODE && mTunnelHelper)
            mTunnelHelper->videoSyncQueueVideoFrame(timestamp,input.size());
    }
}

std::deque<std::unique_ptr<C2Work>>::iterator C2VdecComponent::findPendingWorkByBitstreamId(
        int32_t bitstreamId) {
    return std::find_if(mPendingWorks.begin(), mPendingWorks.end(),
            [bitstreamId](const std::unique_ptr<C2Work>& w) {
            return frameIndexToBitstreamId(w->input.ordinal.frameIndex) ==
            bitstreamId;
            });
}

std::deque<std::unique_ptr<C2Work>>::iterator C2VdecComponent::findPendingWorkByMediaTime(
        int64_t mediaTime) {
    return std::find_if(mPendingWorks.begin(), mPendingWorks.end(),
            [mediaTime](const std::unique_ptr<C2Work>& w) {
            return w->input.ordinal.timestamp.peekull() ==
            mediaTime;
            });
}

C2Work* C2VdecComponent::getPendingWorkByBitstreamId(int32_t bitstreamId) {
    auto workIter = findPendingWorkByBitstreamId(bitstreamId);
    if (workIter == mPendingWorks.end()) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1,"[%s] Can't find pending work by bitstream ID: %d", __func__, bitstreamId);
        return nullptr;
    }
    return workIter->get();
}

C2Work* C2VdecComponent::getPendingWorkByMediaTime(int64_t mediaTime) {
    auto workIter = findPendingWorkByMediaTime(mediaTime);
    if (workIter == mPendingWorks.end()) {
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s] Can't find pending work by mediaTime: %" PRId64"", __func__, mediaTime);
        return nullptr;
    }
    return workIter->get();
}

C2VdecComponent::GraphicBlockInfo* C2VdecComponent::getGraphicBlockById(int32_t blockId) {
    auto blockIter = std::find_if(mGraphicBlocks.begin(), mGraphicBlocks.end(),
        [blockId](const GraphicBlockInfo& gb) {
            return gb.mBlockId == blockId;
    });

    if (blockIter == mGraphicBlocks.end()) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s] Get GraphicBlock Failed: blockId=%d", __func__, blockId);
        return nullptr;
    }

    return &(*blockIter);
}

C2VdecComponent::GraphicBlockInfo* C2VdecComponent::getGraphicBlockByBlockId(uint32_t poolId,uint32_t blockId) {
    auto blockIter = std::find_if(mGraphicBlocks.begin(), mGraphicBlocks.end(),
            [poolId,blockId](const GraphicBlockInfo& gb) {
                if (gb.mPoolId == poolId) {
                    return gb.mBlockId == blockId;
                }
                return false;
        });

    if (blockIter == mGraphicBlocks.end()) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s] Get GraphicBlock Failed: poolId=%u", __func__, poolId);
        return nullptr;
    }
    return &(*blockIter);
}

bool C2VdecComponent::isResolutionChanging () {
    {
        AutoMutex l(mResolutionChangingLock);
        return mResolutionChanging;
    }
}

bool C2VdecComponent::IsCompHaveCurrentBlock(uint32_t poolId,uint32_t blockId) {
    auto blockIter = std::find_if(mGraphicBlocks.begin(), mGraphicBlocks.end(),
            [poolId,blockId](const GraphicBlockInfo& gb) {
                if (gb.mPoolId == poolId) {
                    return gb.mBlockId == blockId;
                }
                return false;
        });

    if (blockIter == mGraphicBlocks.end()) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s] current block fetch done: blockId=%u", __func__, blockId);
        return false;
    }
    return true;
}

bool C2VdecComponent::IsCheckStopDequeueTask() {
    bool ret = false;
    uint64_t nowTimeMs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000000;
    //we reuse this to check reset is complete or not it timeout will force reset once time
    if (mComponentState == ComponentState::FLUSHING &&
        mLastFlushTimeMs > 0 && nowTimeMs - mLastFlushTimeMs >= 2000) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "flush timeout we need force flush once time");
        mLastFlushTimeMs = 0;
        if (mVideoDecWraper != nullptr)
            mVideoDecWraper->stop(RESET_FLAG_NOWAIT);
    } else if (mComponentState == ComponentState::STOPPING) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "[%s:%d] component state is STOPPING,not need to fatch new block", __func__, __LINE__);
        ret = true;
    }

    if (mReportEosWork == true && (mComponentState == ComponentState::STOPPING || mComponentState == ComponentState::DESTROYING || mComponentState == ComponentState::DESTROYED)) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "[%s:%d] component report eos work,exit thread", __func__, __LINE__);
        ret = true;
    }
    return ret;
}

C2VdecComponent::GraphicBlockInfo* C2VdecComponent::getGraphicBlockByFd(int32_t fd) {
    auto blockIter = std::find_if(mGraphicBlocks.begin(), mGraphicBlocks.end(),
            [fd](const GraphicBlockInfo& gb) {
            return gb.mFd == fd;
            });

    if (blockIter == mGraphicBlocks.end()) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s] Get GraphicBlock Failed: fd=%u", __func__, fd);
        return nullptr;
    }
    return &(*blockIter);
}

std::deque<C2VdecComponent::OutputBufferInfo>::iterator C2VdecComponent::findPendingBuffersToWorkByTime(uint64_t timeus) {
    return std::find_if(mPendingBuffersToWork.begin(), mPendingBuffersToWork.end(),
            [time=timeus](const OutputBufferInfo& o) {
                return o.mMediaTimeUs == time;});
}

bool C2VdecComponent::erasePendingBuffersToWorkByTime(uint64_t timeus) {
   auto buffer = findPendingBuffersToWorkByTime(timeus);
   if (buffer != mPendingBuffersToWork.end()) {
       mPendingBuffersToWork.erase(buffer);
   }

   return C2_OK;
}


C2VdecComponent::GraphicBlockInfo* C2VdecComponent::getUnbindGraphicBlock() {
    auto blockIter = std::find_if(mGraphicBlocks.begin(), mGraphicBlocks.end(),
            [&](const GraphicBlockInfo& gb) {
            return gb.mBind == false;
            });
    if (blockIter == mGraphicBlocks.end()) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "GetUnbindGraphicBlock Failed\n");
        return nullptr;
    }
    return &(*blockIter);
}

void C2VdecComponent::onOutputFormatChanged(std::unique_ptr<VideoFormat> format) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s:%d]New output format(pixel_format=0x%x, min_num_buffers=%u, coded_size=%s, crop_rect=%s)",
            __func__, __LINE__,
            static_cast<uint32_t>(format->mPixelFormat), format->mMinNumBuffers,
            format->mCodedSize.ToString().c_str(), format->mVisibleRect.ToString().c_str());

    mCanQueueOutBuffer = false;
    for (auto& info : mGraphicBlocks) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Index:%d,graphic block status:%s count:%ld",
                __func__, __LINE__,
                info.mBlockId, GraphicBlockState(info.mState), info.mGraphicBlock.use_count());
        if (info.mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR) {
            GraphicBlockInfo* info1 = (GraphicBlockInfo*)&info;
            GraphicBlockStateChange(this, info1, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
            BufferStatus(this, CODEC2_LOG_TAG_BUFFER, "outformat change reset index=%d", info1->mBlockId);
        }
    }

    if (mDeviceUtil != nullptr) {
        mDeviceUtil->checkConfigInfoFromDecoderAndReconfig(INTERLACE);
        if (mDeviceUtil->checkUseP010Mode() != kUnUseP010) {
            mDeviceUtil->checkConfigInfoFromDecoderAndReconfig(YCBCR_P010_STREAM);
        }
    }
    CHECK(!mPendingOutputFormat);
    mPendingOutputFormat = std::move(format);
    tryChangeOutputFormat();
}


void C2VdecComponent::updateOutputDelayBufCount() {
    uint32_t dequeueBufferNum = 0;
    if (mOutputFormat.mMinNumBuffers > kDefaultSmoothnessFactor) {
        dequeueBufferNum = mOutputFormat.mMinNumBuffers - kDefaultSmoothnessFactor;
    } else {
        //default add one buf for output delay count
        dequeueBufferNum = 1;
    }
    if (!mUseBufferQueue) {
        dequeueBufferNum = mOutputFormat.mMinNumBuffers;
    }

    int32_t bufferNumAdd = property_get_int32(C2_PROPERTY_VDEC_OUT_ADD_DELAY, 0);
    dequeueBufferNum += bufferNumAdd;
    C2Vdec_LOG(CODEC2_LOG_INFO, "Update dequeue buffer num: %d -> %d out delay buffer margin:%d", mOutputFormat.mMinNumBuffers, dequeueBufferNum, bufferNumAdd);

    C2PortActualDelayTuning::output outputDelay(dequeueBufferNum);
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    c2_status_t outputDelayErr = mIntfImpl->config({&outputDelay}, C2_MAY_BLOCK, &failures);
    if (outputDelayErr == OK) {
        outputDelay.value = dequeueBufferNum;
        mOutputDelay = std::make_shared<C2PortActualDelayTuning::output>(std::move(outputDelay));
    } else {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Update dequeueBufferNum %d error", dequeueBufferNum);
    }
    //update mOutputDelay
    if (mOutputDelay != nullptr) {
        std::unique_ptr<C2Work> work(new C2Work);
        work->result = C2_OK;
        work->input.flags =  (C2FrameData::flags_t)0;
        work->worklets.clear();
        work->worklets.emplace_back(new C2Worklet);
        work->worklets.front()->output.flags = C2FrameData::FLAG_INCOMPLETE;
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*(mOutputDelay)));
        reportWork(std::move(work));
        mOutputDelay = nullptr;
    } else {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Update mOutputDelay is null dequeueBufferNum %d error", dequeueBufferNum);
    }
}

void C2VdecComponent::tryChangeOutputFormat() {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "TryChangeOutputFormat");
    if (mPendingOutputFormat == nullptr) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "TryChangeOutputFormat return mPendingOutputFormat is reset");
        return;
    }
    CHECK(mPendingOutputFormat);
    // || mComponentState == ComponentState::FLUSHING
    if (isNonTunnelMode() && (!mPendingBuffersToWork.empty())) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Pending buffers has work, and wait...");
        mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::tryChangeOutputFormat,
                                            ::base::Unretained(this)));
        return;
    }

    // At this point, all output buffers should not be owned by accelerator. The component is not
    // able to know when a client will release all owned output buffers by now. But it is ok to
    // leave them to client since component won't own those buffers anymore.
    //  TODO: we may also set a parameter for component to keep dequeueing buffers and
    //                 change format only after the component owns most buffers. This may prevent
    //                 too many buffers are still on client's hand while component starts to
    //                 allocate more buffers. However, it leads latency on output format change.
    for (const auto& info : mGraphicBlocks) {
        if (info.mState == GraphicBlockInfo::State::OWNED_BY_ACCELERATOR) {
            C2Vdec_LOG(CODEC2_LOG_ERR, "Graphic block (id:%d) should not be owned by accelerator while changing format",
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
        mResChStat = C2_RESOLUTION_CHANGING;
    }

    //CHECK_EQ(mPendingOutputFormat->mPixelFormat, HalPixelFormat::YCRCB_420_SP);

    mLastOutputFormat.mPixelFormat = mOutputFormat.mPixelFormat;
    mLastOutputFormat.mMinNumBuffers =  mOutputFormat.mMinNumBuffers;
    mLastOutputFormat.mCodedSize = mOutputFormat.mCodedSize;

    mOutputFormat.mPixelFormat = mPendingOutputFormat->mPixelFormat;
    mOutputFormat.mMinNumBuffers = mPendingOutputFormat->mMinNumBuffers;
    mOutputFormat.mCodedSize = mPendingOutputFormat->mCodedSize;

    setOutputFormatCrop(mPendingOutputFormat->mVisibleRect);

    {
        AutoMutex l(mResolutionChangingLock);
        mResolutionChanging = false;
    }

    if (isNonTunnelMode()) {
      updateOutputDelayBufCount();
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

c2_status_t C2VdecComponent::videoResolutionChange() {
    C2Vdec_LOG(CODEC2_LOG_INFO, "VideoResolutionChange");

    bool bufferSizeChanged = false;
    bool bufferNumIncreased = false;
    mPendingOutputFormat.reset();
    if (mDequeueThreadUtil)
        mDequeueThreadUtil->StopRunDequeueTask();

    auto reallocate = mDeviceUtil->isReallocateOutputBuffer(mLastOutputFormat, mOutputFormat, &bufferSizeChanged, &bufferNumIncreased);
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "output buffer reallocate:%d size change:%d number increase:%d", reallocate, bufferSizeChanged, bufferNumIncreased);

    if (mBlockPoolUtil->isBufferQueue()) {
        if (bufferSizeChanged) {
            for (auto& info : mGraphicBlocks) {
                info.mFdHaveSet = false;
                info.mBind = false;
                info.mGraphicBlock.reset();
                mBlockPoolUtil->resetGraphicBlock(info.mBlockId);
                C2Vdec_LOG(CODEC2_LOG_INFO, "Change reset block id:%d, count:%ld", info.mBlockId, info.mGraphicBlock.use_count());
                info.mBlockId = -1;
                info.mFd = -1;
                info.mPoolId = 0;
                if (mPendingGraphicBlockBuffer) {
                    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Change reset pending block id: %d, count:%ld",
                        mPendingGraphicBlockBufferId, mPendingGraphicBlockBuffer.use_count());
                    mBlockPoolUtil->resetGraphicBlock(mPendingGraphicBlockBufferId);
                    mPendingGraphicBlockBufferId = -1;
                    mPendingGraphicBlockBuffer.reset();
                }
                GraphicBlockInfo *info1 = &info;
                GraphicBlockStateReset(this, info1);
            }

            mGraphicBlocks.clear();
            mBlockPoolUtil->cancelAllGraphicBlock();
            size_t inc_buf_num = mOutputFormat.mMinNumBuffers - mLastOutputFormat.mMinNumBuffers;
            size_t bufferCount = mOutputFormat.mMinNumBuffers + kDpbOutputBufferExtraCount;
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Increase buffer num:%d graphic blocks size: %d", (int)inc_buf_num, (int)mGraphicBlocks.size());
            auto err = mBlockPoolUtil->requestNewBufferSet(static_cast<int32_t>(bufferCount));
            if (err != C2_OK) {
                C2Vdec_LOG(CODEC2_LOG_ERR, "Failed to request new buffer set to block pool: %d", err);
                reportError(err);
            }
            if (mVideoDecWraper) {
                mVideoDecWraper->assignPictureBuffers(mOutputFormat.mMinNumBuffers);
            }
        } else if (bufferNumIncreased) {
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s:%d] Do not need realloc", __func__, __LINE__);
            if (mVideoDecWraper) {
                mVideoDecWraper->assignPictureBuffers(mOutputFormat.mMinNumBuffers);
            }
            for (auto& info : mGraphicBlocks) {
                info.mFdHaveSet = false;
                if (info.mState == GraphicBlockInfo::State::OWNED_BY_COMPONENT) {
                    sendOutputBufferToAccelerator(&info, true /* ownByAccelerator */);
                }
            }
        } else {
            // The size and number of buffers in the current decoder have not changed,
            // so it is unnecessary to reallocate buffers.
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Component assign the number of buffer to decoder now.");
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
        if (!mDequeueThreadUtil->StartRunDequeueTask(mOutputFormat.mCodedSize, static_cast<uint32_t>(mOutputFormat.mPixelFormat))) {
            C2Vdec_LOG(CODEC2_LOG_ERR, "[%s:%d] StartDequeueThread Failed", __func__, __LINE__);
            reportError(C2_CORRUPTED);
            return C2_CORRUPTED;
        }
    } else {
        if (reallocate) {
            for (auto& info : mGraphicBlocks) {
                mBlockPoolUtil->resetGraphicBlock(info.mBlockId);
                info.mFdHaveSet = false;
                info.mBind = false;
                info.mBlockId = -1;
                info.mGraphicBlock.reset();
            }

            mGraphicBlocks.clear();
            mBlockPoolUtil->cancelAllGraphicBlock();

            resetBlockPoolUtil();
            size_t inc_buf_num = mOutputFormat.mMinNumBuffers - mLastOutputFormat.mMinNumBuffers;
            size_t bufferCount = mOutputFormat.mMinNumBuffers + kDpbOutputBufferExtraCount;
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Increase buffer num:%d graphic blocks size:%d buffer count:%d", (int)inc_buf_num, (int)mGraphicBlocks.size(), (int)bufferCount);
            auto err = mBlockPoolUtil->requestNewBufferSet(static_cast<int32_t>(bufferCount));
            if (err != C2_OK) {
                C2Vdec_LOG(CODEC2_LOG_ERR, "Failed to request new buffer set to block pool: %d", err);
                reportError(err);
            }

            if (mVideoDecWraper) {
                mVideoDecWraper->assignPictureBuffers(mOutputFormat.mMinNumBuffers);
            }

            if (!mDequeueThreadUtil->StartRunDequeueTask(mOutputFormat.mCodedSize, static_cast<uint32_t>(mOutputFormat.mPixelFormat))) {
                C2Vdec_LOG(CODEC2_LOG_ERR, "[%s:%d] StartDequeueThread Failed", __func__, __LINE__);
                reportError(C2_CORRUPTED);
                return C2_CORRUPTED;
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

            if (!mDequeueThreadUtil->StartRunDequeueTask(mOutputFormat.mCodedSize, static_cast<uint32_t>(mOutputFormat.mPixelFormat))) {
                C2Vdec_LOG(CODEC2_LOG_ERR, "[%s:%d] StartDequeueThread Failed", __func__, __LINE__);
                reportError(C2_CORRUPTED);
                return C2_CORRUPTED;
            }
        }
    }

    mCurrentBlockSize = mOutputFormat.mCodedSize;
    mCurrentPixelFormat = static_cast<uint32_t>(mOutputFormat.mPixelFormat);
    uint32_t frameDur = mDeviceUtil->getVideoDurationUs();
    mDequeueThreadUtil->StartAllocBuffer();
    for (int i = 1; i <= mOutputFormat.mMinNumBuffers; i++) {
        mDequeueThreadUtil->postDelayedAllocTask(mCurrentBlockSize, mCurrentPixelFormat, true, i * frameDur);
    }
    //update picture size
    C2StreamPictureSizeInfo::output videoSize(0u, mOutputFormat.mVisibleRect.width(), mOutputFormat.mVisibleRect.height());
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    c2_status_t err = mIntfImpl->config({&videoSize}, C2_MAY_BLOCK, &failures);
    mPictureSizeChanged = true;
    mCurrentSize = std::make_shared<C2StreamPictureSizeInfo::output>(0u, mOutputFormat.mVisibleRect.width(),
            mOutputFormat.mVisibleRect.height());
    if (err != OK) {
       C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Video size changed, update to params fail");
    }

    return C2_OK;
}

int C2VdecComponent::getDefaultMaxBufNum(InputCodec videotype) {
    int defaultMaxBuffers = 16;

    if (videotype == InputCodec::AV1) {
        defaultMaxBuffers = 16;
    } else if (videotype == InputCodec::VP9) {
        defaultMaxBuffers = 14;
    } else if (videotype == InputCodec::H265) {
        defaultMaxBuffers = 12;
    } else if (videotype == InputCodec::H264) {
        defaultMaxBuffers = 12;
    } else if (videotype == InputCodec::MP2V) {
        defaultMaxBuffers = 12;
    } else if (videotype == InputCodec::MP4V) {
        defaultMaxBuffers = 12;
    }

    return defaultMaxBuffers;
}

bool C2VdecComponent::getVideoResolutionChanged() {
    if (mResChStat == C2_RESOLUTION_CHANGING) {
        for (auto& info : mGraphicBlocks) {
            if (info.mFdHaveSet == false)
                return false;
        }
        mResChStat = C2_RESOLUTION_CHANGED;
        C2Vdec_LOG(CODEC2_LOG_INFO, "Video resolution changed Successfully");
    }

    return true;
}

c2_status_t C2VdecComponent::reallocateBuffersForUsageChanged(const media::Size& size,
                                                              uint32_t pixelFormat) {
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "ReallocateBuffers(%s, 0x%x)", size.ToString().c_str(), pixelFormat);

    // Get block pool ID configured from the client.
    uint64_t poolId = -1;
    c2_status_t err;
    if (mBlockPoolUtil == nullptr) {
        std::shared_ptr<C2BlockPool> blockPool = nullptr;
        poolId = mIntfImpl->getBlockPoolId();
        err = GetCodec2BlockPool(poolId, shared_from_this(), &blockPool);
        if (err != C2_OK) {
            C2Vdec_LOG(CODEC2_LOG_ERR, "Graphic block allocator is invalid");
            reportError(err);
            return err;
        }
        DCHECK(blockPool != NULL);
        mBlockPoolUtil = std::make_shared<C2VdecBlockPoolUtil> (blockPool);
    }

    if (mBlockPoolUtil->isBufferQueue()) {
        mUseBufferQueue = true;
        CODEC2_LOG(CODEC2_LOG_INFO, "Using C2BlockPool ID:%" PRId64" for allocating output buffers, blockpooolId:%d", poolId, mBlockPoolUtil->getAllocatorId());
    } else {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Graphic block allocator is invalid");
        reportError(C2_CORRUPTED);
        return C2_CORRUPTED;
    }

    mGraphicBlocks.clear();
    size_t bufferCount = mOutputFormat.mMinNumBuffers + kDpbOutputBufferExtraCount;
    size_t minBuffersForDisplay = 0;
    // Set requested buffer count to C2BlockPool.
    err = mBlockPoolUtil->requestNewBufferSet(static_cast<int32_t>(bufferCount));
    if (err != C2_OK) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Failed to request new buffer set to block pool: %d", err);
        reportError(err);
        return err;
    }
    err = mBlockPoolUtil->getMinBuffersForDisplay(&minBuffersForDisplay);
    if (err != C2_OK) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Failed to query minimum undequeued buffer count from block pool: %d", err);
        reportError(err);
        return err;
    }
    int64_t surfaceUsage = mBlockPoolUtil->getConsumerUsage();
    if (!(surfaceUsage & GRALLOC_USAGE_HW_COMPOSER)) {
        mDeviceUtil->setUseSurfaceTexture(true);
    } else {
        mDeviceUtil->setUseSurfaceTexture(false);
        mDeviceUtil->setForceFullUsage(true);
    }

    mUndequeuedBlockIds.resize(minBuffersForDisplay, -1);
    C2MemoryUsage usage = {
            mSecureMode ? (C2MemoryUsage::READ_PROTECTED | C2MemoryUsage::WRITE_PROTECTED) :
            (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE), mDeviceUtil->getPlatformUsage()};

    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Minimum undequeued buffer count = %zu  usage= %" PRId64"", minBuffersForDisplay, usage.expected);

    for (size_t i = 0; i < bufferCount; ++i) {
        std::shared_ptr<C2GraphicBlock> block;

        int32_t retries_left = kAllocateBufferMaxRetries;
        err = C2_NO_INIT;
        while (err != C2_OK) {
            C2Fence fence;
            auto format = mDeviceUtil->getStreamPixelFormat(pixelFormat);
            err = mBlockPoolUtil->fetchGraphicBlock(mDeviceUtil->getOutAlignedSize(size.width()),
                                               mDeviceUtil->getOutAlignedSize(size.height()),
                                               format, usage, &block, &fence);
            if (err == C2_TIMED_OUT && retries_left > 0) {
                C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Allocate buffer timeout, %d retry time(s) left...", retries_left);
                retries_left--;
            } else if (err != C2_OK) {
                mGraphicBlocks.clear();
                C2Vdec_LOG(CODEC2_LOG_ERR, "Failed to allocate buffer: %d", err);
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
            C2Vdec_LOG(CODEC2_LOG_ERR, "Get the block id failed, please check it. err:%d", err);
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
        if (info == nullptr) {
            C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] got null blockinfo, donot use it", __func__);
            return C2_OK;
        }
        sendOutputBufferToAccelerator(info, true /*ownByAccelerator*/);
    }

    mOutputFormat.mMinNumBuffers = bufferCount;
    if (isNonTunnelMode() && !mDequeueThreadUtil->StartRunDequeueTask(size, pixelFormat)) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "[%s:%d] startDequeueThread Failed", __func__, __LINE__);
        reportError(C2_CORRUPTED);
        return C2_CORRUPTED;
    }
    return C2_OK;
}

void C2VdecComponent::resetBlockPoolUtil() {

    mBlockPoolUtil.reset();
    mBlockPoolUtil = nullptr;

    std::shared_ptr<C2BlockPool> blockPool = NULL;
    auto poolId = mIntfImpl->getBlockPoolId();
    auto err = GetCodec2BlockPool(poolId, shared_from_this(), &blockPool);
    if (err != C2_OK || !blockPool) {
        CODEC2_LOG(CODEC2_LOG_INFO, "Get block pool ok, id:%" PRId64 "", poolId);
        err = CreateCodec2BlockPool(poolId, shared_from_this(), &blockPool);
        if (err != C2_OK) {
            C2Vdec_LOG(CODEC2_LOG_ERR, "Graphic block allocator is invalid");
            reportError(err);
        }
    }
    DCHECK(blockPool != NULL);
    mUseBufferQueue = blockPool->getAllocatorId() == C2PlatformAllocatorStore::BUFFERQUEUE;
    mBlockPoolUtil = std::make_shared<C2VdecBlockPoolUtil> (blockPool);
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Reset block pool util success.");
}

c2_status_t C2VdecComponent::allocNonTunnelBuffers(const media::Size& size, uint32_t pixelFormat) {
    size_t bufferCount = mOutBufferCount;
    int64_t surfaceUsage = 0;
    size_t minBuffersForDisplay = 0;
    C2BlockPool::local_id_t poolId = -1;

    mBlockPoolUtil->requestNewBufferSet(static_cast<int32_t>(bufferCount));
    c2_status_t err = mBlockPoolUtil->getMinBuffersForDisplay(&minBuffersForDisplay);
    if (err != C2_OK) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Graphic block allocator is invalid");
        reportError(err);
        return err;
    }
    //check is can play 8k normally
    bool support = mDeviceUtil->checkSupport8kMode();
    if (support == false) {
        reportError(C2_NO_MEMORY);
        return C2_NO_MEMORY;
    }
    mUndequeuedBlockIds.resize(minBuffersForDisplay, -1);
    uint64_t platformUsage = mDeviceUtil->getPlatformUsage();
    C2MemoryUsage usage = {
            mSecureMode ? (C2MemoryUsage::READ_PROTECTED | C2MemoryUsage::WRITE_PROTECTED) :
            (C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE), platformUsage};
    // The number of buffers requested for the first time is the number defined in the framework.
    int32_t dequeue_buffer_num = 2 + kDefaultSmoothnessFactor;
    if (!mUseBufferQueue) {
        dequeue_buffer_num = bufferCount;
    }
    //fixed android.mediav2.cts.CodecDecoderSurfaceTest#testFlushNative[28(c2.amlogic.mpeg2.decoder_video/mpeg2)]
    //android.mediav2.cts.CodecDecoderSurfaceTest#testFlushNative[29(c2.amlogic.mpeg2.decoder_video/mpeg2)]
    //do not change dequeue buf count too big when codec is mpeg2 and used bufferqueue.
    //out buf release is slow at surface mode,
    //we cannot get out buf so fast.
    if (mDeviceUtil->isInterlaced() &&
        !(mIntfImpl->getCodecProfile() == media::MPEG2_PROFILE && mBlockPoolUtil->isBufferQueue())) {
        if (mOutBufferCount > kDefaultSmoothnessFactor)
            dequeue_buffer_num = mOutBufferCount - kDefaultSmoothnessFactor;
    }

    if (dequeue_buffer_num > bufferCount) {
        //first alloc count can not > total count,this occur decoder need
        //too few buffer count.
        dequeue_buffer_num = bufferCount;
    }
    if (dequeue_buffer_num < 2 * mOutBufferCount / 3) {
        dequeue_buffer_num = 2 * mOutBufferCount / 3;
    }
    int32_t llv_first_alloc_num = mOutBufferCount / 2;
    if (mDeviceUtil->isLowLatencyMode() && (dequeue_buffer_num > llv_first_alloc_num)) {
        llv_first_alloc_num = property_get_int32(C2_PROPERTY_VDEC_LLV_FIRST_ALLOC_NUM, llv_first_alloc_num);
        CODEC2_LOG(CODEC2_LOG_INFO, "lowlatency mode set first alloc buffer num from %d to %d", dequeue_buffer_num, llv_first_alloc_num);
        dequeue_buffer_num = llv_first_alloc_num;
    }

    // Allocate the output buffers.
    if (mVideoDecWraper) {
        if (mDeviceUtil->checkUseP010Mode() == kUseHardwareP010) {
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] Hardware p010 use NV12", __func__);
            mVideoDecWraper->setOutputFormat(V4L2_PIX_FMT_NV12);
        }
        mVideoDecWraper->assignPictureBuffers(bufferCount);
    }
    mCanQueueOutBuffer = true;

    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Minimum undequeued buffer count:%zu buffer count:%d first_bufferNum:%d Usage %" PRId64"",
                minBuffersForDisplay, (int)bufferCount, dequeue_buffer_num, usage.expected);
    for (int i = 0; i < dequeue_buffer_num; ++i) {
        std::shared_ptr<C2GraphicBlock> block;

        int32_t retries_left = kAllocateBufferMaxRetries;
        err = C2_NO_INIT;
        while (err != C2_OK) {
            if (mIsReleasing)
                return C2_OK;
            auto format = mDeviceUtil->getStreamPixelFormat(pixelFormat);
            C2Fence fence;
            err = mBlockPoolUtil->fetchGraphicBlock(mDeviceUtil->getOutAlignedSize(size.width()),
                                            mDeviceUtil->getOutAlignedSize(size.height()),
                                            format, usage, &block, &fence);
            if (err == C2_TIMED_OUT && retries_left > 0) {
                C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Allocate buffer timeout, %d retry time(s) left...", retries_left);
                if (retries_left == kAllocateBufferMaxRetries && mUseBufferQueue) {
                    int64_t newSurfaceUsage = mBlockPoolUtil->getConsumerUsage();
                    if (newSurfaceUsage != surfaceUsage) {
                        return reallocateBuffersForUsageChanged(size, format);
                    }
                }
                retries_left--;
            } else if (err == EAGAIN) {
                C2Vdec_LOG(CODEC2_LOG_INFO, "Failed to allocate buffer: %d retry i = %d", err, i);
                ::usleep(kDequeueRetryDelayUs);
                break;
            } else if (err != C2_OK) {
                C2Vdec_LOG(CODEC2_LOG_INFO, "[%s@%d] Failed to allocate buffer, state: %d", __func__, __LINE__, err);
                ::usleep(kDequeueRetryDelayUs);
                //reportError(err);
                //return err;
                //break;
            }
        }
        if (err == EAGAIN) {
            dequeue_buffer_num = i;
            break;
        }

        poolId = -1;
        uint32_t blockId = 0;
        mBlockPoolUtil->getPoolId(&poolId);
        err = mBlockPoolUtil->getBlockIdByGraphicBlock(block, &blockId);

        if (err != C2_OK) {
            mGraphicBlocks.clear();
            C2Vdec_LOG(CODEC2_LOG_ERR, "Get the block id failed, please check it. err:%d", err);
            reportError(err);
            return err;
        }

        appendOutputBuffer(std::move(block), poolId, blockId, true);
        GraphicBlockInfo *info = getGraphicBlockByBlockId(poolId, blockId);
        if (info == nullptr) {
            C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] got null blockinfo, donot use it", __func__);
            return C2_OK;
        }
        GraphicBlockStateInit(this, info, GraphicBlockInfo::State::OWNED_BY_COMPONENT);
        BufferStatus(this, CODEC2_LOG_TAG_BUFFER, "initialize alloc outbuf index=%d", info->mBlockId);
        sendOutputBufferToAccelerator(info, true /*ownByAccelerator*/);
    }
    //
    updateOutputDelayBufCount();
    mOutputFormat.mMinNumBuffers = bufferCount;
    int dequeueTaskCount = bufferCount - dequeue_buffer_num;
    float frameRate = mIntfImpl->getInputFrameRate();
    int frameDur = (int) (1000 / frameRate) * 1000;

    if (!mDequeueThreadUtil->StartRunDequeueTask(size, pixelFormat)) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "[%s:%d] StartDequeueThread Failed", __func__, __LINE__);
        reportError(C2_CORRUPTED);
        return C2_CORRUPTED;
    }
    mDequeueThreadUtil->StartAllocBuffer();

    for (int i = 1; i <= dequeueTaskCount; i++) {
        mDequeueThreadUtil->postDelayedAllocTask(size, pixelFormat, true, static_cast<uint32_t>(i * frameDur + 10000));
    }

    mCurrentBlockSize = size;
    mCurrentPixelFormat = pixelFormat;
    mOutputFormat.mMinNumBuffers = bufferCount;
    return C2_OK;
}

c2_status_t C2VdecComponent::allocateBuffersFromBlockPool(const media::Size& size,
                                                              uint32_t pixelFormat) {
    C2Vdec_LOG(CODEC2_LOG_INFO, "AllocateBuffersFromBlockPool(%s, 0x%x)", size.ToString().c_str(), pixelFormat);
    mDequeueThreadUtil->StopRunDequeueTask();
    size_t bufferCount = mOutputFormat.mMinNumBuffers + kDpbOutputBufferExtraCount;
    if (isTunnelMode() || !mDeviceUtil->needAllocWithMaxSize()) {
        mOutBufferCount = getDefaultMaxBufNum(GetIntfImpl()->getInputCodec());
        if (bufferCount > mOutBufferCount) {
            C2Vdec_LOG(CODEC2_LOG_INFO, "required outbuffer count %d large than default num %d", (int)bufferCount, mOutBufferCount);
            mOutBufferCount = bufferCount;
        } else {
            bufferCount = mOutBufferCount;
        }
    }
    mOutBufferCount = bufferCount;
    mGraphicBlocks.clear();

    // Get block pool ID configured from the client.
    std::shared_ptr<C2BlockPool> blockPool;
    C2BlockPool::local_id_t poolId = 0;
    c2_status_t err;
    if (mBlockPoolUtil == nullptr) {
        poolId = mIntfImpl->getBlockPoolId();
        err = GetCodec2BlockPool(poolId, shared_from_this(), &blockPool);
        if (err != C2_OK) {
            C2Vdec_LOG(CODEC2_LOG_ERR, "Graphic block allocator is invalid");
            reportError(err);
            return err;
        }
        CODEC2_LOG(CODEC2_LOG_INFO,"Using C2BlockPool ID:%" PRId64 "for allocating output buffers, allocator id:%d",
                poolId, blockPool->getAllocatorId());
        DCHECK(blockPool != NULL);
        mBlockPoolUtil = std::make_shared<C2VdecBlockPoolUtil> (blockPool);
    }

    int64_t surfaceUsage = 0;
    bool usersurfacetexture = false;
    if (mBlockPoolUtil->isBufferQueue()) {
        mUseBufferQueue = true;
        surfaceUsage = mBlockPoolUtil->getConsumerUsage();
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Get block pool usage:%" PRId64 "", surfaceUsage);
        if (!(surfaceUsage & GRALLOC_USAGE_HW_COMPOSER)) {
            usersurfacetexture = true;
            mDeviceUtil->setUseSurfaceTexture(true);
        }
    } else {
        if (isNonTunnelMode()) {
            mDeviceUtil->setNoSurface(true);
        }
    }
    mHDR10PlusMeteDataNeedCheck = true;

    if (isNonTunnelMode()) {
        allocNonTunnelBuffers(size, pixelFormat);
    } else if (isTunnelMode() && mTunnelHelper){
        mHDR10PlusMeteDataNeedCheck = true;
        mTunnelHelper->allocTunnelBuffersAndSendToDecoder(size, pixelFormat);
    }

    return C2_OK;
}

void C2VdecComponent::appendOutputBuffer(std::shared_ptr<C2GraphicBlock> block, uint32_t poolId,uint32_t blockId, bool bind) {
    GraphicBlockInfo info;
    int fd = 0;
    auto err = mBlockPoolUtil->getBlockFd(block, &fd);
    if (err != C2_OK) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Get the block fd failed. err:%d", err);
        reportError(err);
        return;
    }

    if (bind) {
        info.mPoolId  = poolId;
        info.mBlockId = blockId;
        info.mGraphicBlock = std::move(block);
        info.mFd = fd;
    }
    info.mBind = bind;
    info.mFdHaveSet = false;

    if (info.mGraphicBlock != NULL) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] graphicblock: %p,fd:%d blockid: %d, size: %dx%d bind %d->%d GraphicBlockSize:%zu", __func__, info.mGraphicBlock->handle(), fd,
            info.mBlockId, info.mGraphicBlock->width(), info.mGraphicBlock->height(), info.mPoolId, info.mBlockId, mGraphicBlocks.size());
    }
    mGraphicBlocks.push_back(std::move(info));
}

void C2VdecComponent::sendOutputBufferToAccelerator(GraphicBlockInfo* info, bool ownByAccelerator) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    if (info == nullptr) {
        return;
    }
    if (ownByAccelerator) {
        if (info->mState != GraphicBlockInfo::State::OWNED_BY_COMPONENT) {
            C2Vdec_LOG(CODEC2_LOG_ERR, "block state error,please theck it.");
        }
        GraphicBlockStateChange(this, info, GraphicBlockInfo::State::OWNED_BY_ACCELERATOR);
    }

    int32_t width = 0, height = 0;
    if (info->mGraphicBlock != nullptr) {
        width  = info->mGraphicBlock->width();
        height = info->mGraphicBlock->height();
    } else {
        C2Vdec_LOG(CODEC2_LOG_ERR, "info->mGraphicBlock is nullptr, please theck it.");
        return;
    }

    CODEC2_VDEC_ATRACE(TRACE_NAME_SEND_OUTPUT_BUFFER.str().c_str(), info->mBlockId);
    BufferStatus(this, CODEC2_LOG_TAG_BUFFER, "send to videodec index=%d, ownByAccelerator=%d, blocksize(%dx%d) formatsize(%dx%d)",
                info->mBlockId, ownByAccelerator, width, height,
                mOutputFormat.mCodedSize.width(), mOutputFormat.mCodedSize.height());

    // mHandles is not empty for the first time the buffer is passed to Vdec. In that case, Vdec needs
    // to import the buffer first.
    if (!info->mFdHaveSet) {
        uint8_t* vaddr = NULL;
        uint32_t size = 0;
        bool isNV21 = true;
        int metaFd =-1;
      if (mBlockPoolUtil->getAllocatorId() == C2PlatformAllocatorStore::BUFFERQUEUE) {
            const native_handle_t* c2Handle = info->mGraphicBlock->handle();
            const native_handle_t* handle = UnwrapNativeCodec2GrallocHandle(c2Handle);
            if (handle != nullptr)
                mDeviceUtil->updateDisplayInfoToGralloc(handle, mDeviceUtil->getVideoType(), mSessionID);
        }
        if (mVideoDecWraper) {
            if (mDeviceUtil->checkUseP010Mode() == kUseHardwareP010) {
                isNV21 = false;
                C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] isNV21:%d", __func__, isNV21);
            }
            mVideoDecWraper->importBufferForPicture(info->mBlockId, info->mFd,
                    metaFd, vaddr, size, isNV21);
            info->mFdHaveSet = true;
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] Fd:%d, id:%d, usecount:%ld", __func__, info->mFd, info->mBlockId, info->mGraphicBlock.use_count());
        }
    } else {
        if (mVideoDecWraper) {
            mVideoDecWraper->reusePictureBuffer(info->mBlockId);
        }
    }
}

bool C2VdecComponent::parseCodedColorAspects(const C2ConstLinearBlock& input) {
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
        C2Vdec_LOG(CODEC2_LOG_ERR, "H264 AdvanceToNextNALU error: %d", static_cast<int>(parRes));
        return false;
    }
    if (nalu.nal_unit_type != media::H264NALU::kSPS) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, ("NALU is not SPS");
        return false;
    }

    int spsId;
    parRes = h264Parser->ParseSPS(&spsId);
    if (parRes != media::H264Parser::kEOStream && parRes != media::H264Parser::kOk) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "H264 ParseSPS error: %d", static_cast<int>(parRes));
        return false;
    }

    // Parse ISO color aspects from H264 SPS bitstream.
    const media::H264SPS* sps = h264Parser->GetSPS(spsId);
    if (!sps->colour_description_present_flag) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, ("No Color Description in SPS");
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
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, ("Parsed ColorAspects from bitstream: (R:%d, P:%d, M:%d, T:%d)", colorAspects.mRange,
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
        C2Vdec_LOG(CODEC2_LOG_ERR, "Failed to config color aspects to interface, error: %d", status);
        return false;
    }
 #endif
    return true;
}

c2_status_t C2VdecComponent::updateColorAspects() {
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "UpdateColorAspects");
    std::unique_ptr<C2StreamColorAspectsInfo::output> colorAspects =
            std::make_unique<C2StreamColorAspectsInfo::output>(
                    0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                    C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED);
    c2_status_t status = mIntfImpl->query({colorAspects.get()}, {}, C2_DONT_BLOCK, nullptr);
    if (status != C2_OK) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Failed to query color aspects, error: %d", status);
        return status;
    }
    mCurrentColorAspects = std::move(colorAspects);
    return C2_OK;
}

c2_status_t C2VdecComponent::updateHDRStaticInfo() {
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "UpdateHDRStaticInfo");
    std::unique_ptr<C2StreamHdrStaticInfo::output> hdr =
        std::make_unique<C2StreamHdrStaticInfo::output>();
    c2_status_t err = mIntfImpl->query({hdr.get()}, {}, C2_DONT_BLOCK, nullptr);
    if (err != C2_OK) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Failed to query hdr static info, error: %d", err);
        return err;
    }
    mCurrentHdrStaticInfo = std::move(hdr);
    return C2_OK;
}
void C2VdecComponent::updateHDR10PlusInfo() {
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "UpdateHDR10PlusInfo");
    std::string hdr10Data;
    if (mDeviceUtil->getHDR10PlusData(hdr10Data)) {
        if (hdr10Data.size() != 0) {
            //std::memcpy(mCurrentHdr10PlusInfo->m.value, hdr10Data.c_str(), hdr10Data.size());
            //mCurrentHdr10PlusInfo->setFlexCount(hdr10Data.size());
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Get HDR10Plus data size:%d ", (int)hdr10Data.size());
        }
    }
}

void C2VdecComponent::onVisibleRectChanged(const media::Rect& cropRect) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "OnVisibleRectChanged");
    RETURN_ON_UNINITIALIZED_OR_ERROR();

    // We should make sure there is no pending output format change. That is, the input cropRect is
    // corresponding to current output format.
    CHECK(mPendingOutputFormat == nullptr);
    setOutputFormatCrop(cropRect);
    //update picture size
    C2StreamPictureSizeInfo::output videoSize(0u, mOutputFormat.mVisibleRect.width(), mOutputFormat.mVisibleRect.height());
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    c2_status_t err = mIntfImpl->config({&videoSize}, C2_MAY_BLOCK, &failures);
    mPictureSizeChanged = true;
    mCurrentSize = std::make_shared<C2StreamPictureSizeInfo::output>(0u, mOutputFormat.mVisibleRect.width(),
            mOutputFormat.mVisibleRect.height());
    if (err != OK) {
       C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Video size changed, update to params fail");
    }
}

void C2VdecComponent::setOutputFormatCrop(const media::Rect& cropRect) {
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "SetOutputFormatCrop(%dx%d)", cropRect.width(), cropRect.height());
    // This visible rect should be set as crop window for each C2ConstGraphicBlock passed to
    // framework.
    mOutputFormat.mVisibleRect = cropRect;
}

void C2VdecComponent::onCheckVideoDecReconfig() {
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2,"[%s]",__func__);
    char mInstanceName[32];
    if (mSurfaceUsageGot)
        return;

    if (mBlockPoolUtil == nullptr) {
        std::shared_ptr<C2BlockPool> blockPool = NULL;
        auto poolId = mIntfImpl->getBlockPoolId();
        auto err = GetCodec2BlockPool(poolId, shared_from_this(), &blockPool);
        if (err != C2_OK || !blockPool) {
            CODEC2_LOG(CODEC2_LOG_INFO, "Get block pool ok, id:%" PRId64 "", poolId);
            err = CreateCodec2BlockPool(poolId, shared_from_this(), &blockPool);
            if (err != C2_OK) {
                C2Vdec_LOG(CODEC2_LOG_ERR, "Graphic block allocator is invalid");
                reportError(err);
            }
        }
        DCHECK(blockPool != NULL);
        mUseBufferQueue = blockPool->getAllocatorId() == C2PlatformAllocatorStore::BUFFERQUEUE;
        mBlockPoolUtil = std::make_shared<C2VdecBlockPoolUtil> (blockPool);
        if (mBlockPoolUtil->isBufferQueue()) {
            C2Vdec_LOG(CODEC2_LOG_INFO, "Bufferqueue-backed block pool is used. blockPool->getAllocatorId() %d, C2PlatformAllocatorStore::BUFFERQUEUE %d",
                blockPool->getAllocatorId(), C2PlatformAllocatorStore::BUFFERQUEUE);
        } else {
            C2Vdec_LOG(CODEC2_LOG_INFO, "Bufferpool-backed block pool is used.");
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
                mVideoDecWraper->setSessionID((uint32_t)mSessionID);
            }
            //check again
            usage = mBlockPoolUtil->getConsumerUsage();
            if (!(usage & GRALLOC_USAGE_HW_COMPOSER)) {
                mDeviceUtil->setUseSurfaceTexture(usersurfacetexture);
            }
            mDeviceUtil->codecConfig(&mConfigParam);
            uint32_t vdecFlags = AM_VIDEO_DEC_INIT_FLAG_CODEC2;
            if (mIntfImpl->mVdecWorkMode->value == VDEC_STREAMMODE)
                vdecFlags |= AM_VIDEO_DEC_INIT_FLAG_STREAMMODE;
            if (mIntfImpl->mDataSourceType->value == DATASOURCE_DMX)
                vdecFlags |= AM_VIDEO_DEC_INIT_FLAG_DMXDATA_SOURCE;
            if (mDeviceUtil->isLowLatencyMode()) {
                vdecFlags |= AM_VIDEO_DEC_INIT_FLAG_USE_LOW_LATENCY_MODE;
            }
            snprintf(mInstanceName, sizeof(mInstanceName), "CODEC2-%d", mSessionID);
            mVdecInitResult = (VideoDecodeAcceleratorAdaptor::Result)mVideoDecWraper->initialize(VideoCodecProfileToMime(mIntfImpl->getCodecProfile()),
                        (uint8_t*)&mConfigParam, sizeof(mConfigParam), mSecureMode, this, vdecFlags, mInstanceName, QuitEventFunc, (void *)this);
            //set some decoder config
            //set unstable state and duration to vdec
            mDeviceUtil->setUnstable();
            mDeviceUtil->setDuration();
            mDecoderID = mVideoDecWraper->getDecoderID();
            TraceInit();
        }
    } else {
        //use BUFFERPOOL, no surface
        if (mVideoDecWraper) {
            mVideoDecWraper->destroy();
        } else {
            mVideoDecWraper = std::make_shared<VideoDecWraper>();
            mVideoDecWraper->setSessionID((uint32_t)mSessionID);
        }
        if (isNonTunnelMode()) {
            mDeviceUtil->setNoSurface(true);
        }
        mDeviceUtil->codecConfig(&mConfigParam);
        uint32_t vdecflags = AM_VIDEO_DEC_INIT_FLAG_CODEC2;
        if (mIntfImpl->mVdecWorkMode->value == VDEC_STREAMMODE)
            vdecflags |= AM_VIDEO_DEC_INIT_FLAG_STREAMMODE;
        if (mIntfImpl->mDataSourceType->value == DATASOURCE_DMX)
            vdecflags |= AM_VIDEO_DEC_INIT_FLAG_DMXDATA_SOURCE;
        if (mDeviceUtil->isLowLatencyMode()) {
            vdecflags |= AM_VIDEO_DEC_INIT_FLAG_USE_LOW_LATENCY_MODE;
        }
        snprintf(mInstanceName, sizeof(mInstanceName), "CODEC2-%d", mSessionID);
        mVdecInitResult = (VideoDecodeAcceleratorAdaptor::Result)mVideoDecWraper->initialize(VideoCodecProfileToMime(mIntfImpl->getCodecProfile()),
                  (uint8_t*)&mConfigParam, sizeof(mConfigParam), mSecureMode, this, vdecflags, mInstanceName, QuitEventFunc, (void *)this);
        //set some decoder config
        //set unstable state and duration to vdec
        mDeviceUtil->setUnstable();
        mDeviceUtil->setDuration();
        mDecoderID = mVideoDecWraper->getDecoderID();
        TraceInit();
        prctl(PR_SET_NAME, (unsigned long) TRACE_NAME_VDEC_COMPONENT_THREAD.str().c_str());
    }

    mSurfaceUsageGot = true;
}

c2_status_t C2VdecComponent::queue_nb(std::list<std::unique_ptr<C2Work>>* const items) {
    if (mState.load() != State::RUNNING) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "Queue_nb State[%d].", mState.load());
        return C2_BAD_STATE;
    }

    if (!mSurfaceUsageGot) {
        if (isNonTunnelMode())
            mTaskRunner->PostTask(FROM_HERE,
                        ::base::Bind(&C2VdecComponent::onCheckVideoDecReconfig, ::base::Unretained(this)));
    }

    while (!items->empty()) {
        CODEC2_ATRACE_CALL();
        mHasQueuedWork = true;
        std::shared_ptr<C2StreamHdrDynamicMetadataInfo::input> info((mIntfImpl->getHdr10PlusInfo()));
        mTaskRunner->PostTask(FROM_HERE,
                              ::base::Bind(&C2VdecComponent::onQueueWork, ::base::Unretained(this),
                                           ::base::Passed(&items->front()),
                                           std::move(info)));
        //onQueueWork(std::move(items->front()));
        items->pop_front();
    }
    return C2_OK;
}

c2_status_t C2VdecComponent::announce_nb(const std::vector<C2WorkOutline>& items) {
    UNUSED(items);
    return C2_OMITTED;  // Tunneling is not supported by now
}

c2_status_t C2VdecComponent::flush_sm(flush_mode_t mode,
                                     std::list<std::unique_ptr<C2Work>>* const flushedWork) {
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] flush mode:%d", __func__, mode);
    if (mode != FLUSH_COMPONENT) {
        return C2_OMITTED;  // Tunneling is not supported by now
    }
    if (mState.load() != State::RUNNING) {
        return C2_BAD_STATE;
    }
    if (!mHasQueuedWork && !mTunerPassthroughHelper) {
        return C2_OK;
    }

    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onFlush,
                                                ::base::Unretained(this)));

    AutoMutex l(mFlushDoneLock);
    if (mFlushDoneCond.waitRelative(mFlushDoneLock, 500000000ll) == ETIMEDOUT) {  // 500ms Time out
        updateComponentState(ComponentState::STARTED);
        uint64_t nowTimeMs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000000;
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] last flush time:%" PRId64", now time:%" PRId64"", __func__, mLastFlushTimeMs, nowTimeMs);
        return C2_TIMED_OUT;
    }

    {
        AutoMutex l(mFlushDoneWorkLock);
        while (!mFlushPendingWorkList.empty()) {
            flushedWork->emplace_back(std::move(mFlushPendingWorkList.front()));
            mFlushPendingWorkList.pop_front();
        }
    }

    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] flush work size:%zd Queue size:%zd  blocksize:%s mCurrentPixelFormat:%d", __func__,
                flushedWork->size(), mQueue.size(), mCurrentBlockSize.ToString().c_str(), mCurrentPixelFormat);

    // passthrough mode has no data flow in C2
    if (!mTunerPassthroughHelper) {
        // Work dequeueing was stopped while component flushing. Restart it.
        mTaskRunner->PostTask(FROM_HERE,
                              ::base::Bind(&C2VdecComponent::onDequeueWork, ::base::Unretained(this)));
        mHaveFlushDone = true;
        mFlushDoneWithOutEosWork = true;
        // if ((mDequeueThreadUtil != nullptr) && isNonTunnelMode()) {
        //     int bufferInClient = mGraphicBlockStateCount[(int)GraphicBlockInfo::State::OWNED_BY_CLIENT];
        //     CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] %d buffer in client and post dequeue task", __func__, bufferInClient);
        //     uint32_t frameDur = mDeviceUtil->getVideoDurationUs();
        //     if (!mDequeueThreadUtil->StartRunDequeueTask(mOutputFormat.mCodedSize, static_cast<uint32_t>(mOutputFormat.mPixelFormat))) {
        //             C2Vdec_LOG(CODEC2_LOG_ERR, "[%s:%d] StartDequeueThread Failed", __func__, __LINE__);
        //     }
        //     mDequeueThreadUtil->StartAllocBuffer();
        //     for (int i = 1; i <= bufferInClient; i++) {
        //         mDequeueThreadUtil->postDelayedAllocTask(mCurrentBlockSize, mCurrentPixelFormat, true, static_cast<uint32_t>(i * frameDur));
        //     }
        // }
    }
    return C2_OK;
}

c2_status_t C2VdecComponent::drain_nb(drain_mode_t mode) {
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] drain mode:%d", __func__, mode);
    if (mode != DRAIN_COMPONENT_WITH_EOS && mode != DRAIN_COMPONENT_NO_EOS) {
        return C2_OMITTED;  // Tunneling is not supported by now
    }
    if (mState.load() != State::RUNNING) {
        return C2_BAD_STATE;
    }
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VdecComponent::onDrain, ::base::Unretained(this),
                                       static_cast<uint32_t>(mode)));
    return C2_OK;
}

c2_status_t C2VdecComponent::start() {
    // Use mStartStopLock to block other asynchronously start/stop calls.
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s]",__func__);
    std::lock_guard<std::mutex> lock(mStartStopLock);

    if (mState.load() != State::LOADED) {
        return C2_BAD_STATE;  // start() is only supported when component is in LOADED state.
    }

    mCodecProfile = mIntfImpl->getCodecProfile();
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Get parameter: mCodecProfile = %d", static_cast<int>(mCodecProfile));

    ::base::WaitableEvent done(::base::WaitableEvent::ResetPolicy::AUTOMATIC,
                               ::base::WaitableEvent::InitialState::NOT_SIGNALED);
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VdecComponent::onStart, ::base::Unretained(this),
                                       mCodecProfile, &done));
    done.Wait();
    c2_status_t c2Status;
    if (mVdecInitResult == VideoDecodeAcceleratorAdaptor::Result::PLATFORM_FAILURE) {
        // Regard unexpected Vdec initialization failure as no more resources, because we still don't
        // have a formal way to obtain the max capable number of concurrent decoders.
        c2Status = C2_NO_MEMORY;
    } else {
        c2Status = adaptorResultToC2Status(mVdecInitResult);
    }

    if (c2Status != C2_OK) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Failed to start component due to Vdec error...");
        return c2Status;
    }
    mState.store(State::RUNNING);
    if (mDebugUtil) {
        mDebugUtil->startShowPipeLineBuffer();
    }
    mTaskRunner->PostDelayedTask(FROM_HERE,
        ::base::Bind(&C2VdecComponent::checkPreempting, ::base::Unretained(this)),
        ::base::TimeDelta::FromMilliseconds(100));
    if (mIntfImpl->mVendorGameModeLatency->enable && mDeviceUtil) {
        mDeviceUtil->setGameMode(true);
    }
    mVdecComponentStopDone = false;
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] done",__func__);
    return C2_OK;
}

// Stop call should be valid in all states (even in error).
c2_status_t C2VdecComponent::stop() {
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s]",__func__);
    // Use mStartStopLock to block other asynchronously start/stop calls.
    std::lock_guard<std::mutex> lock(mStartStopLock);

    if (mVdecComponentStopDone) {
        return C2_CANNOT_DO;
    }

    auto state = mState.load();
    if (!(state == State::RUNNING || state == State::ERROR)) {
        return C2_OK;  // Component is already in stopped state.
    }
    if (mIntfImpl->mVendorGameModeLatency->enable && mDeviceUtil) {
        mDeviceUtil->setGameMode(false);
    }

    ::base::WaitableEvent done(::base::WaitableEvent::ResetPolicy::AUTOMATIC,
                               ::base::WaitableEvent::InitialState::NOT_SIGNALED);
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VdecComponent::onStop, ::base::Unretained(this), &done));
    done.Wait();
    mState.store(State::LOADED);
    mVdecComponentStopDone = true;

    if (mStopDoneEvent != nullptr)
        mStopDoneEvent = nullptr;

    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] done",__func__);
    return C2_OK;
}

c2_status_t C2VdecComponent::reset() {
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s]",__func__);
    c2_status_t ret = C2_OK;
    mVdecComponentStopDone = false;
    ret = stop();
    if (ret == C2_CANNOT_DO) {
        ret = C2_OK;
    }
    return ret;
    //  TODO: reset is different than stop that it could be called in any state.
    //  TODO: when reset is called, set ComponentInterface to default values.
}

c2_status_t C2VdecComponent::release() {
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s]",__func__);
    c2_status_t ret = C2_OK;
    mIsReleasing = true;
    ret = reset();
    if (mDebugUtil) {
        removeObserver(mDebugUtil);
        mDebugUtil->dtor();
        mDebugUtil.reset();
        mDebugUtil = NULL;
    }
    if (mThread.IsRunning()) {
        ::base::WaitableEvent done(::base::WaitableEvent::ResetPolicy::AUTOMATIC,
                                   ::base::WaitableEvent::InitialState::NOT_SIGNALED);
        mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onDestroy,
                                ::base::Unretained(this), &done));
        done.Wait();
        mThread.Stop();
    }
    mIsReleasing = false;
    return ret;
}

std::shared_ptr<C2ComponentInterface> C2VdecComponent::intf() {
    return mIntf;
}

void C2VdecComponent::ProvidePictureBuffers(uint32_t minNumBuffers, uint32_t width, uint32_t height) {
    // Always use flexible pixel 420 format YCbCr_420_888 in Android.
    // Uses coded size for crop rect while it is not available.
    {
        AutoMutex l(mResolutionChangingLock);
        mResolutionChanging = true;
    }

    if (mBufferFirstAllocated && minNumBuffers < mOutputFormat.mMinNumBuffers)
        minNumBuffers = mOutputFormat.mMinNumBuffers;

    int32_t douleWrite = mDeviceUtil->getDoubleWriteModeValue();
    int32_t tripleWrite = mDeviceUtil->getTripleWriteModeValue();

    if (douleWrite == 0 && tripleWrite == 0) {
        width = 64;
        height = 64;
    }

    uint32_t max_width = width;
    uint32_t max_height = height;

    mDeviceUtil->queryStreamBitDepth();
    mDeviceUtil->checkUseP010Mode();

    if (!mDeviceUtil->needAllocWithMaxSize()) {
        mDeviceUtil->getMaxBufWidthAndHeight(max_width, max_height);
    }
    auto format = std::make_unique<VideoFormat>(HalPixelFormat::YCRCB_420_SP, minNumBuffers,
                                                media::Size(max_width, max_height), media::Rect(width, height));

    // Set mRequestedVisibleRect to default.
    mRequestedVisibleRect = media::Rect();
    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onOutputFormatChanged,
                                                  ::base::Unretained(this),
                                                  ::base::Passed(&format)));
}

void C2VdecComponent::DismissPictureBuffer(int32_t pictureBufferId) {
    UNUSED(pictureBufferId);
    // no ops
}

void C2VdecComponent::PictureReady(int32_t pictureBufferId, int64_t bitstreamId,
                                  uint32_t x, uint32_t y, uint32_t w, uint32_t h, int32_t flags) {
    UNUSED(pictureBufferId);
    UNUSED(bitstreamId);

    if (mRequestedVisibleRect != media::Rect(x, y, w, h)) {
        mRequestedVisibleRect = media::Rect(x, y, w, h);
        mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onVisibleRectChanged,
                                                      ::base::Unretained(this), media::Rect(x, y, w, h)));
    }

    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onOutputBufferDone,
                                                  ::base::Unretained(this),
                                                  pictureBufferId, bitstreamId, flags, 0));
}

void C2VdecComponent::PictureReady(output_buf_param_t* params) {

    int32_t pictureBufferId = params->pictureBufferId;
    int64_t bitstreamId = params->bitstreamId;
    uint32_t x = params->x;
    uint32_t y = params->y;
    uint32_t w = params->width;
    uint32_t h = params->height;
    int32_t flags = params->flags;
    uint64_t timestamp = params->timestamp;

    if (mRequestedVisibleRect != media::Rect(x, y, w, h)) {
        mRequestedVisibleRect = media::Rect(x, y, w, h);
        mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onVisibleRectChanged,
                                                      ::base::Unretained(this), media::Rect(x, y, w, h)));
    }

    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onOutputBufferDone,
                                                   ::base::Unretained(this),
                                                   pictureBufferId, bitstreamId, flags, timestamp));
}

void C2VdecComponent::UpdateDecInfo(const uint8_t* info, uint32_t isize) {
    UNUSED(info);
    UNUSED(isize);
    struct aml_dec_params* pInfo = (struct aml_dec_params*)info;
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "C2VdecComponent::UpdateDecInfo, dec_parms_status=%d\n", pInfo->parms_status);
    mDeviceUtil->updateDecParmInfo(pInfo);
}


void C2VdecComponent::NotifyEndOfBitstreamBuffer(int32_t bitstreamId) {
    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::onInputBufferDone,
                                                  ::base::Unretained(this), bitstreamId));
}

void C2VdecComponent::NotifyFlushDone() {
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s]", __func__);
    mDequeueThreadUtil->StopAllocBuffer();
    mHaveDrainDone = true;
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VdecComponent::onDrainDone, ::base::Unretained(this)));
}

void C2VdecComponent::NotifyFlushOrStopDone() {
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s]", __func__);
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VdecComponent::onFlushOrStopDone, ::base::Unretained(this)));
}

void C2VdecComponent::onReportError(c2_status_t error) {
    C2Vdec_LOG(CODEC2_LOG_ERR, "[%s]", __func__);
    if (mComponentState == ComponentState::DESTROYED) {
        return;
    }
    reportError(error);
}

void C2VdecComponent::NotifyError(int error) {
    C2Vdec_LOG(CODEC2_LOG_ERR, "Got notifyError from Vdec...");
    c2_status_t err = adaptorResultToC2Status((VideoDecodeAcceleratorAdaptor::Result)error);
    if (err == C2_OK) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Shouldn't get SUCCESS err code in NotifyError(). Skip it...");
        return;
    }
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VdecComponent::onReportError, ::base::Unretained(this), err));
}

void C2VdecComponent::onNoOutFrameNotify(int64_t bitstreamId) {
    mNoOutFrameWorkQueue.push_back(bitstreamId);
    mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VdecComponent::onReportNoOutFrameFinished, ::base::Unretained(this)));
}

void C2VdecComponent::onReportNoOutFrameFinished() {
    while (!mNoOutFrameWorkQueue.empty()) {
        int64_t bitstreamId = mNoOutFrameWorkQueue.front();
        auto workIter = findPendingWorkByBitstreamId(bitstreamId);
        if (workIter == mPendingWorks.end()) {
            C2Vdec_LOG(CODEC2_LOG_ERR, "[%s:%d] Can not find work with bistreamId:%lld", __func__, __LINE__, (long long)bitstreamId);
            reportError(C2_CORRUPTED);
            return;
        }

        auto work = workIter->get();
        if (!isNoOutFrameDone(bitstreamId, work)) {
            C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] no outframe work with bistreamId %lld not finished, will retry", __func__, __LINE__, (long long)bitstreamId);
            return;
        }
        work->result = C2_OK;
        work->workletsProcessed = static_cast<uint32_t>(work->worklets.size());
        std::list<std::unique_ptr<C2Work>> finishedWorks;
        finishedWorks.emplace_back(std::move(*workIter));
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s:%d] Reported finished work index=%llu",__func__,__LINE__, work->input.ordinal.frameIndex.peekull());
        CODEC2_VDEC_ATRACE(TRACE_NAME_FINISHED_WORK_PTS.str().c_str(), work->input.ordinal.timestamp.peekull());
        mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
        CODEC2_VDEC_ATRACE(TRACE_NAME_FINISHED_WORK_PTS.str().c_str(), 0);;
        mPendingWorks.erase(workIter);
        mOutputFinishedWorkCount++;
        mNoOutFrameWorkQueue.pop_front();
    }
}

void C2VdecComponent::NotifyEvent(uint32_t event, void *param, uint32_t paramsize) {
    UNUSED(param);
    UNUSED(paramsize);
    int32_t bitstreamId = -1;
    switch (event) {
        case VideoDecWraper::FIELD_INTERLACED:
            CODEC2_LOG(CODEC2_LOG_INFO, "Is interlaced");
            mDeviceUtil->updateInterlacedInfo(true);
            break;
        case VideoDecWraper::FRAME_ERROR:
            bitstreamId = *(int32_t *)(param);
            mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VdecComponent::onErrorFrameWorksAndReportIfFinised, ::base::Unretained(this), bitstreamId));
            break;
        case VideoDecWraper::FARAME_INCOMPLETE:
            bitstreamId = *(uint32_t *)(param);
            mTaskRunner->PostTask(FROM_HERE,
                          ::base::Bind(&C2VdecComponent::onNoOutFrameNotify, ::base::Unretained(this), bitstreamId));
            break;
        default:
            CODEC2_LOG(CODEC2_LOG_INFO, "NotifyEvent:event:%d", event);
            break;
    }
}

void C2VdecComponent::onErrorFrameWorksAndReportIfFinised(int32_t bitstreamId) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());

    if (bitstreamId < 0) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "bitstream id is error,please check it.");
        return;
    }

    auto work = getPendingWorkByBitstreamId(bitstreamId);
    if (!work) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "[%s:%d] Can not get pending work with bitstreamId:%d", __func__, __LINE__,  bitstreamId);
        return;
    }

    work->worklets.front()->output.flags = C2FrameData::FLAG_DROP_FRAME;
    mErrorFrameWorkCount++;
    CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d]if current work finish input buffer done,so discard bitstream id:%d count:%" PRId64 "", __func__, __LINE__,  bitstreamId, mErrorFrameWorkCount);
    reportWorkIfFinished(bitstreamId, 0);
}

void C2VdecComponent::detectNoShowFrameWorksAndReportIfFinished(
        const C2WorkOrdinalStruct& currOrdinal) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());

    {
        for (auto& work : mPendingWorks) {
            // A work in mPendingWorks would be considered to have no-show frame if there is no
            // corresponding output buffer returned while the one of the work with latter timestamp is
            // already returned. (Vdec is outputted in display order.)
            if (isNoShowFrameWork(*(work.get()), currOrdinal)) {
                // Mark FLAG_DROP_FRAME for no-show frame work.
                work->worklets.front()->output.flags = C2FrameData::FLAG_DROP_FRAME;

                // We need to call reportWorkIfFinished() for all detected no-show frame works. However,
                // we should do it after the detection loop since reportWorkIfFinished() may erase
                // entries in mPendingWorks.
                int32_t bitstreamId = frameIndexToBitstreamId(work->input.ordinal.frameIndex);
                mNoShowFrameBitstreamIds.push_back(bitstreamId);
                C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "Detected no-show frame work index=%llu timestamp=%llu",
                           work->input.ordinal.frameIndex.peekull(), work->input.ordinal.timestamp.peekull());
            }
        }
    }
    mTaskRunner->PostTask(FROM_HERE, ::base::Bind(&C2VdecComponent::reportWorkForNoShowFrames, ::base::Unretained(this)));
}

void C2VdecComponent::reportWorkForNoShowFrames() {
    for (int32_t bitstreamId : mNoShowFrameBitstreamIds) {
        // Try to report works with no-show frame.
        reportWorkIfFinished(bitstreamId,0);
    }
    mNoShowFrameBitstreamIds.clear();
}

bool C2VdecComponent::isNoShowFrameWork(const C2Work& work,
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

void C2VdecComponent::reportEmptyWork(int32_t bitstreamId, int32_t flags) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    auto workIter = findPendingWorkByBitstreamId(bitstreamId);
    if (workIter == mPendingWorks.end()) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "ReportEmptyWork findPendingWorkByBitstreamId Failed. bitstreamId:%d",bitstreamId);
        return;
    }
    auto work = workIter->get();
    work->result = C2_OK;
    work->worklets.front()->output.buffers.clear();
    work->workletsProcessed = 1;
    work->input.ordinal.customOrdinal = mDeviceUtil->getLastOutputPts();

    c2_cntr64_t timestamp = work->worklets.front()->output.ordinal.timestamp + work->input.ordinal.customOrdinal
                            - work->input.ordinal.timestamp;
    C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "ReportEmptyWork index=%llu pts=%llu,%d", work->input.ordinal.frameIndex.peekull(), timestamp.peekull(),__LINE__);

    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "ReportEmptyWork: This is EOS work and should be processed by reportEOSWork().");
    } else if (work->input.buffers.front()) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "ReportEmptyWork:  Input buffer is still owned by Vdec.");
    } else if (mPendingOutputEOS && mPendingWorks.size() == 1u) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "ReportEmptyWork:  If mPendingOutputEOS is true, the last returned work should be marked EOS flag and returned by reportEOSWork() instead.");
    }

    std::list<std::unique_ptr<C2Work>> finishedWorks;
    finishedWorks.emplace_back(std::move(*workIter));
    mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
    mPendingWorks.erase(workIter);
    mOutputFinishedWorkCount++;
}

void C2VdecComponent::reportWork(std::unique_ptr<C2Work> work) {
    std::list<std::unique_ptr<C2Work>> finishedWorks;
    work->result = C2_OK;
    finishedWorks.emplace_back(std::move(work));
    mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
}

bool C2VdecComponent::isNoOutFrameDone(int64_t bitstreamId, const C2Work* work) {
    if (mNoOutFrameWorkQueue.empty()) {
        return false;
    }

    if (work->input.buffers.front()) {
        return false;
    }

    auto iter = std::find(mNoOutFrameWorkQueue.begin(), mNoOutFrameWorkQueue.end(), bitstreamId);
    if (iter == mNoOutFrameWorkQueue.end()) {
        return false;
    }

    return true;
}

c2_status_t C2VdecComponent::reportWorkIfFinished(int32_t bitstreamId, int32_t flags, bool isEmptyWork) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());

    auto workIter = findPendingWorkByBitstreamId(bitstreamId);
    if (workIter == mPendingWorks.end()) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "[%s:%d] Can not find work with bistreamId:%d", __func__, __LINE__, bitstreamId);
        reportError(C2_CORRUPTED);
        return C2_OK;
    }

    // EOS work will not be reported here. reportEOSWork() does it.
    auto work = workIter->get();
    if (isEmptyWork || isWorkDone(work)) {
        CODEC2_ATRACE_CALL();
        if (work->worklets.front()->output.flags & C2FrameData::FLAG_DROP_FRAME) {
            // A work with neither flags nor output buffer would be treated as no-corresponding
            // output by C2 framework, and regain pipeline capacity immediately.
            //  TODO: output FLAG_DROP_FRAME flag after it could be handled correctly.
            work->worklets.front()->output.flags = static_cast<C2FrameData::flags_t>(0);
        }
        work->result = C2_OK;
        work->workletsProcessed = static_cast<uint32_t>(work->worklets.size());
        //save last out pts
        mDeviceUtil->setLastOutputPts(work->input.ordinal.customOrdinal.peekull());
        //work->input.ordinal.customOrdinal = mDeviceUtil->checkAndAdjustOutPts(work, flags);
        c2_cntr64_t timestamp = work->worklets.front()->output.ordinal.timestamp + work->input.ordinal.customOrdinal
                                - work->input.ordinal.timestamp;
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s:%d] Reported finished work index=%llu pts=%llu", __func__, __LINE__,
            work->input.ordinal.frameIndex.peekull(), timestamp.peekull());
        std::list<std::unique_ptr<C2Work>> finishedWorks;
        finishedWorks.emplace_back(std::move(*workIter));
        CODEC2_VDEC_ATRACE(TRACE_NAME_FINISHED_WORK_PTS.str().c_str(), work->input.ordinal.timestamp.peekull());
        mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
        CODEC2_VDEC_ATRACE(TRACE_NAME_FINISHED_WORK_PTS.str().c_str(), 0);
        mPendingWorks.erase(workIter);
        mOutputFinishedWorkCount++;
        return C2_OK;
    }
    return C2_CANNOT_DO;
}

bool C2VdecComponent::isInputWorkDone(const C2Work* work) const {
    if (work->input.buffers.front()) {
        // Input buffer is still owned by Vdec.
        return false;
    }
    return true;  // This work is input done.
}


bool C2VdecComponent::isWorkDone(const C2Work* work) const {
    if (work->input.buffers.front() && !mIntfImpl->mVendorGameModeLatency->enable) {
        // Input buffer is still owned by Vdec.
        return false;
    }

    if (mPendingOutputEOS && mPendingWorks.size() == 1u) {
        // If mPendingOutputEOS is true, the last returned work should be marked EOS flag and
        // returned by reportEOSWork() instead.
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL2, "isWorkDone eos false.");
        return false;
    }

    if (!(work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) &&
            !(work->worklets.front()->output.flags & C2FrameData::FLAG_DROP_FRAME)) {
        // Unless the input is CSD or the output is dropped, this work is not done because the
        // output buffer is not returned from Vdec yet.
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

bool C2VdecComponent::isNonTunnelMode() const {
    return (mSyncType == C2_SYNC_TYPE_NON_TUNNEL);
}

uint32_t C2VdecComponent::getBitDepthByColorAspects() {
    uint32_t bitdepth = 8;
    std::shared_ptr<C2StreamColorAspectsTuning::output> defaultColorAspects;
    if (GetIntfImpl()->getPixelFormatInfoValue() != HAL_PIXEL_FORMAT_YCBCR_420_888) {
        defaultColorAspects = GetIntfImpl()->getDefaultColorAspects();
        if (defaultColorAspects->primaries == C2Color::PRIMARIES_BT2020 &&
            defaultColorAspects->matrix == C2Color::MATRIX_BT2020 &&
            defaultColorAspects->transfer == C2Color::TRANSFER_ST2084) {
            C2Vdec_LOG(CODEC2_LOG_INFO, "default color aspects is RGBA1010102");
            bitdepth = 10;
        }
    }

    CODEC2_LOG(CODEC2_LOG_INFO, "[%s] bitdepth:%d", __func__, bitdepth);
    return bitdepth;
}

bool C2VdecComponent::isTunnelMode() const {
    return (mSyncType == C2_SYNC_TYPE_TUNNEL);
}

bool C2VdecComponent::isTunnerPassthroughMode() const {
    return (mSyncType == (C2_SYNC_TYPE_TUNNEL | C2_SYNC_TYPE_PASSTHROUGH));
}

c2_status_t C2VdecComponent::reportEOSWork() {
    C2Vdec_LOG(CODEC2_LOG_INFO, "reportEOSWork");
    DCHECK(mTaskRunner->BelongsToCurrentThread());

    if (mPendingWorks.empty()) {
        C2Vdec_LOG(CODEC2_LOG_ERR, "Failed to find EOS work.");
        reportError(C2_CORRUPTED);
        return C2_CORRUPTED;
    }
    mReportEosWork = true;
    mPendingOutputEOS = false;
    std::unique_ptr<C2Work> eosWork = std::move(mPendingWorks.back());
    mPendingWorks.pop_back();
    if (!eosWork->input.buffers.empty()) {
        eosWork->input.buffers.front().reset();
    }
    eosWork->result = C2_OK;
    eosWork->workletsProcessed = static_cast<uint32_t>(eosWork->worklets.size());
    eosWork->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;

    if (!mUseBufferQueue) {
        mIsReportEosWork = true;
    }

    if (!mPendingWorks.empty()) {
        C2Vdec_LOG(CODEC2_LOG_INFO, "There are remaining works except EOS work. abandon them.");
        for (const auto& kv : mPendingWorks) {
            C2Vdec_LOG(CODEC2_LOG_INFO, "Work index=%llu, timestamp=%llu",
                  kv->input.ordinal.frameIndex.peekull(),
                  kv->input.ordinal.timestamp.peekull());
        }
        reportAbandonedWorks();
    }

    std::list<std::unique_ptr<C2Work>> finishedWorks;
    finishedWorks.emplace_back(std::move(eosWork));
    mListener->onWorkDone_nb(shared_from_this(), std::move(finishedWorks));
    mOutputFinishedWorkCount++;
    return C2_OK;
}

void C2VdecComponent::reportAbandonedWorks() {
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
        if (!mUseBufferQueue && abandonedWorks.empty() && mIsReportEosWork && isNonTunnelMode()) {
            for (auto & info : mGraphicBlocks) {
                if (info.mState == GraphicBlockInfo::State::OWNED_BY_COMPONENT) {
                    C2ConstGraphicBlock constBlock = info.mGraphicBlock->share(
                                    C2Rect(mOutputFormat.mVisibleRect.width(),
                                    mOutputFormat.mVisibleRect.height()),
                                    C2Fence());
                    std::shared_ptr<C2Buffer> buffer = C2Buffer::CreateGraphicBuffer(std::move(constBlock));
                    work->worklets.front()->output.buffers.emplace_back(std::move(buffer));
                    C2Vdec_LOG(CODEC2_LOG_INFO, "report abandoned work and add block.");
                }
            }
            mIsReportEosWork = false;
        }
        CODEC2_LOG(CODEC2_LOG_INFO, "[%s] %s mode abandon bitstreamid:%d mediatimeus:%llu", __func__,
            (isTunnelMode() ? "tunnel" : "no-tunnel"),
            frameIndexToBitstreamId(work->input.ordinal.frameIndex),
            work->input.ordinal.timestamp.peekull());

        abandonedWorks.emplace_back(std::move(work));
    }

    for (auto& work : mAbandonedWorks) {
        // TODO: correlate the definition of flushed work result to framework.
        work->result = C2_NOT_FOUND;
        // When the work is abandoned, buffer in input.buffers shall reset by component.
        if (!work->input.buffers.empty()) {
            work->input.buffers.front().reset();
        }
        if (mTunnelHelper) {
            mTunnelHelper->storeAbandonedFrame(work->input.ordinal.timestamp.peekull());
        }

        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] %s mode abandon bitstreamid:%d mediatimeus:%llu", __func__,
            (isTunnelMode() ? "tunnel" : "no-tunnel"),
            frameIndexToBitstreamId(work->input.ordinal.frameIndex),
            work->input.ordinal.timestamp.peekull());
        abandonedWorks.emplace_back(std::move(work));
    }
    mAbandonedWorks.clear();

    // Pending EOS work will be abandoned here due to component flush if any.
    mPendingOutputEOS = false;

    if (!abandonedWorks.empty()) {
        mListener->onWorkDone_nb(shared_from_this(), std::move(abandonedWorks));
        mOutputFinishedWorkCount++;
    }
}

void C2VdecComponent::reportError(c2_status_t error) {
    DCHECK(mTaskRunner->BelongsToCurrentThread());
    ALOGV("reportError");
    if (mComponentState == ComponentState::DESTROYING ||
        mComponentState == ComponentState::DESTROYED ||
        mComponentState == ComponentState::UNINITIALIZED) {
        C2Vdec_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s] Have been in destroy or stop state", __func__);
        return;
    }
    mListener->onError_nb(shared_from_this(), static_cast<uint32_t>(error));
    updateComponentState(mComponentState, true);
    mState.store(State::ERROR);
}

const char* C2VdecComponent::GraphicBlockState(GraphicBlockInfo::State state) {
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

const char* C2VdecComponent::VideoCodecProfileToMime(media::VideoCodecProfile profile) {
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
    } else if (profile == media::MPEG2_PROFILE) {
        return "video/mpeg2";
    } else if (profile == media::MJPEG_PROFILE) {
        return "video/mjpeg";
    } else if (profile == media::AVS3_PROFILE) {
        return "video/avs3";
    } else if (profile == media::AVS2_PROFILE) {
        return "video/avs2";
    } else if (profile == media::AVS_PROFILE) {
        return "video/avs";
    } else if (profile == media::VC1_PROFILE) {
        return "video/vc1";
    }
    return "";
}


void C2VdecComponent::onConfigureTunnelMode() {
    /* configure */
    C2Vdec_LOG(CODEC2_LOG_INFO, "[%s] synctype:%d, syncid:%d", __func__, mIntfImpl->mTunnelModeOutput->m.syncType, mIntfImpl->mTunnelModeOutput->m.syncId[0]);
    if (mIntfImpl->mTunnelModeOutput->m.syncType == C2PortTunneledModeTuning::Struct::sync_type_t::AUDIO_HW_SYNC) {
        int syncId = mIntfImpl->mTunnelModeOutput->m.syncId[0];
        if (syncId >= 0) {
            if (((syncId & 0x0000FF00) == 0xFF00)
                || (syncId == 0x0)) {
                mSyncId = syncId;
                if (mTunnelHelper) {
                    removeObserver(mTunnelHelper);
                    mTunnelHelper.reset();
                    mTunnelHelper = NULL;
                }
                mTunnelHelper =  std::make_shared<TunnelHelper>(mSecureMode);
                addObserver(mTunnelHelper, static_cast<int>(mComponentState), mCompHasError);
                mTunnelHelper->setComponent(shared_from_this());
                mSyncType &= (~C2_SYNC_TYPE_NON_TUNNEL);
                mSyncType |= C2_SYNC_TYPE_TUNNEL;
            }
        }
    }

    return;
}

void C2VdecComponent::onConfigureTunerPassthroughMode() {
    if (mTunerPassthroughHelper) {
        removeObserver(mTunerPassthroughHelper);
        mTunerPassthroughHelper.reset();
        mTunerPassthroughHelper = NULL;
    }
    mTunerPassthroughHelper = std::make_shared<TunerPassthroughHelper>(mSecureMode, VideoCodecProfileToMime(mIntfImpl->getCodecProfile()), mTunnelHelper);
    addObserver(mTunerPassthroughHelper, static_cast<int>(mComponentState), mCompHasError);
    mTunerPassthroughHelper->setComponent(shared_from_this());
    mSyncType &= (~C2_SYNC_TYPE_NON_TUNNEL);
    mSyncType |= C2_SYNC_TYPE_PASSTHROUGH;
}

void C2VdecComponent::onConfigureTunerPassthroughTrickMode() {
   mTunerPassthroughHelper->setTrickMode();
}

void C2VdecComponent::onConfigureTunerPassthroughWorkMode() {
   mTunerPassthroughHelper->setWorkMode();
}

void C2VdecComponent::onConfigureTunerPassthroughInstanceNo() {
   mTunerPassthroughHelper->setInstanceNo();
}

void C2VdecComponent::onConfigureTunerPassthroughEventMask() {
   mTunerPassthroughHelper->setRenderCallBackEventFlag();
}

void C2VdecComponent::onConfigureTunerPassthroughMute() {
   mTunerPassthroughHelper->setMute();
}

void C2VdecComponent::onConfigureTunerPassthroughScreenColor() {
   mTunerPassthroughHelper->setScreenColor();
}

void C2VdecComponent::onConfigureTunerPassthroughTransitionModeBefore() {
   mTunerPassthroughHelper->setTransitionModeBefore();
}

void C2VdecComponent::onConfigureTunerPassthroughTransitionModeAfter() {
   mTunerPassthroughHelper->setTransitionModeAfter();
}

void C2VdecComponent::onConfigureTunerPassthroughTransitionPrerollRate() {
   mTunerPassthroughHelper->setTransitionPrerollRate();
}

void C2VdecComponent::onConfigureTunerPassthroughTransitionPrerollAVTolerance() {
   mTunerPassthroughHelper->setTransitionPrerollAVTolerance();
}

void C2VdecComponent::onConfigureTunerPassthroughPlaybackStatus() {
   mTunerPassthroughHelper->setPlaybackStatus();
}

void C2VdecComponent::onConfigureEsModeHwAvsyncId(int32_t avSyncId){
    if (mTunnelHelper) {
        if ((avSyncId & 0x0000FF00) == 0xFF00 || avSyncId == 0x0) {
            mTunnelHelper->configureEsModeHwAvsyncId((avSyncId |(1u << 16)));
        } else {
            CODEC2_LOG(CODEC2_LOG_ERR, "Invalid hwsyncid:0x%x", avSyncId);
        }
     }
}

void C2VdecComponent::updateComponentState(const ComponentState& state, bool error) {
    mComponentState = state;
    mCompHasError = error;
    notifyObservers();
}

void C2VdecComponent::notifyObservers() {
    for (auto& observer : observers) {
        observer->updateState(static_cast<int>(mComponentState));
        observer->updateError(mCompHasError);
    }
}

class C2VdecComponentFactory : public C2ComponentFactory {
public:
    C2VdecComponentFactory(C2String decoderName)
          : mDecoderName(decoderName),
            mReflector(std::static_pointer_cast<C2ReflectorHelper>(
                    GetCodec2VendorComponentStore()->getParamReflector())){};

    c2_status_t createComponent(c2_node_id_t id, std::shared_ptr<C2Component>* const component,
                                ComponentDeleter deleter) override {
        *component = C2VdecComponent::create(mDecoderName, id, mReflector, deleter);
        return *component ? C2_OK : C2_NO_MEMORY;
    }
    c2_status_t createInterface(c2_node_id_t id,
                                std::shared_ptr<C2ComponentInterface>* const interface,
                                InterfaceDeleter deleter) override {
        UNUSED(deleter);
        *interface = std::shared_ptr<C2ComponentInterface>(new SimpleInterface<C2VdecComponent::IntfImpl>(
                    mDecoderName.c_str(), id,
                    std::make_shared<C2VdecComponent::IntfImpl>(mDecoderName, mReflector)));
        return C2_OK;
    }
    ~C2VdecComponentFactory() override = default;

private:
    const C2String mDecoderName;
    std::shared_ptr<C2ReflectorHelper> mReflector;
};
}  // namespace android


#define CreateC2VdecFactory(type) \
    extern "C" ::C2ComponentFactory* CreateC2Vdec##type##Factory(bool secureMode) {\
         ALOGV("create component %s secure:%d", #type, secureMode);\
         return secureMode ? new ::android::C2VdecComponentFactory(android::k##type##SecureDecoderName)\
                            :new ::android::C2VdecComponentFactory(android::k##type##DecoderName);\
    }
#define CreateC2VdecClearFactory(type) \
        extern "C" ::C2ComponentFactory* CreateC2Vdec##type##Factory(bool secureMode) {\
             ALOGV("create component %s secure:%d", #type, secureMode);\
             UNUSED(secureMode);\
             return new ::android::C2VdecComponentFactory(android::k##type##DecoderName);\
        }


#define DestroyC2VdecFactory(type) \
    extern "C" void DestroyC2Vdec##type##Factory(::C2ComponentFactory* factory) {\
        delete factory;\
    }

CreateC2VdecFactory(H264)
CreateC2VdecFactory(H265)
CreateC2VdecFactory(VP9)
CreateC2VdecFactory(AV1)
CreateC2VdecFactory(DVHE)
CreateC2VdecFactory(DVAV)
CreateC2VdecFactory(DVAV1)
CreateC2VdecFactory(MP2V)
CreateC2VdecClearFactory(MP4V)
CreateC2VdecClearFactory(MJPG)
CreateC2VdecClearFactory(AVS3)
CreateC2VdecClearFactory(AVS2)
CreateC2VdecClearFactory(AVS)
CreateC2VdecClearFactory(HWVC1)

DestroyC2VdecFactory(H264)
DestroyC2VdecFactory(H265)
DestroyC2VdecFactory(VP9)
DestroyC2VdecFactory(AV1)
DestroyC2VdecFactory(DVHE)
DestroyC2VdecFactory(DVAV)
DestroyC2VdecFactory(DVAV1)
DestroyC2VdecFactory(MP2V)
DestroyC2VdecFactory(MP4V)
DestroyC2VdecFactory(MJPG)
DestroyC2VdecFactory(AVS3)
DestroyC2VdecFactory(AVS2)
DestroyC2VdecFactory(AVS)
DestroyC2VdecFactory(HWVC1)
