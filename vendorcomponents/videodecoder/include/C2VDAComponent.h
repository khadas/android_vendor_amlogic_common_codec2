// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VDA_COMPONENT_H
#define ANDROID_C2_VDA_COMPONENT_H

#include <VideoDecodeAcceleratorAdaptor.h>

#include <rect.h>
#include <size.h>
#include <video_codecs.h>
#include <video_decode_accelerator.h>

#include <C2Component.h>
#include <C2ComponentFactory.h>
#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>

#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>

#include <base/macros.h>
#include <base/memory/ref_counted.h>
#include <base/single_thread_task_runner.h>
#include <base/synchronization/waitable_event.h>
#include <base/threading/thread.h>
#include <utils/threads.h>

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <queue>
#include <unordered_map>

#include <AmVideoDecBase.h>
#include <VideoDecWraper.h>
#include <VideoTunnelRendererWraper.h>
#include <TunerPassthroughWrapper.h>
#include <C2VendorConfig.h>
#include <C2VDABlockPoolUtil.h>
#include <C2VendorSupport.h>

namespace android {

#define DECLARE_C2_DEFAUTL_UNSTRICT_SETTER(s,n) \
    static C2R n##Setter(bool mayBlock, C2P<s> &me)

class C2VDAComponent : public C2Component,
                       public VideoDecWraper::VideoDecWraperCallback,
                       public std::enable_shared_from_this<C2VDAComponent> {
public:
    class IntfImpl;
    static std::shared_ptr<C2Component> create(const std::string& name, c2_node_id_t id,
                                               const std::shared_ptr<C2ReflectorHelper>& helper,
                                               C2ComponentFactory::ComponentDeleter deleter);

    C2VDAComponent(C2String name, c2_node_id_t id,
                   const std::shared_ptr<C2ReflectorHelper>& helper);
    virtual ~C2VDAComponent() override;

    class MetaDataUtil;
    class TunnelModeHelper;

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

    //mediahal callback
    virtual void ProvidePictureBuffers(uint32_t minNumBuffers,  uint32_t width, uint32_t height);
    virtual void DismissPictureBuffer(int32_t picture_buffer_id);
    virtual void PictureReady(int32_t pictureBufferId, int64_t bitstreamId,
            uint32_t x, uint32_t y, uint32_t w, uint32_t h, int32_t flags);
    virtual void UpdateDecInfo(const uint8_t* info, uint32_t isize);
    virtual void NotifyEndOfBitstreamBuffer(int32_t bitstream_buffer_id);
    virtual void NotifyFlushDone();
    virtual void NotifyFlushOrStopDone();
    virtual void NotifyError(int error);
    virtual void NotifyEvent(uint32_t event, void *param, uint32_t paramsize);

    //config
    void onConfigureTunnelMode();

    //for out use
    IntfImpl* GetIntfImpl() {
       return mIntfImpl.get();
    }

    scoped_refptr<::base::SingleThreadTaskRunner> GetTaskRunner() {
        return mTaskRunner;
    }

    bool isAmDolbyVision() {
        return mIsDolbyVision;
    }

    static uint32_t mInstanceNum;
    static uint32_t mInstanceID;
    uint32_t mCurInstanceID;

    static const uint32_t kUpdateDurationFramesNumMax = 10;
    int mUpdateDurationUsCount;
private:
    friend TunnelModeHelper;
    // The state machine enumeration on parent thread.
    enum class State : int32_t {
        // The initial state of component. State will change to LOADED after the component is
        // created.
        UNLOADED,
        // The component is stopped. State will change to RUNNING when start() is called by
        // framework.
        LOADED,
        // The component is running, State will change to LOADED when stop() or reset() is called by
        // framework.
        RUNNING,
        // The component is in error state.
        ERROR,
    };
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

    enum class PictureFlag : int32_t {
      PICTURE_FLAG_NONE = 0,
      PICTURE_FLAG_KEYFRAME = 0x0001,
      PICTURE_FLAG_PFRAME = 0x0002,
      PICTURE_FLAG_BFRAME = 0x0004,
      PICTURE_FLAG_FIELD_NONE   = 0x0008,
      PICTURE_FLAG_FIELD_TOP    = 0x0010,
      PICTURE_FLAG_FIELD_BOTTOM = 0x0020,
      PICTURE_FLAG_ERROR_FRAME = 0x0100,
    };

    // This constant is used to tell apart from drain_mode_t enumerations in C2Component.h, which
    // means no drain request.
    // Note: this value must be different than all enumerations in drain_mode_t.
    static constexpr uint32_t NO_DRAIN = ~0u;

    // Internal struct for work queue.
    struct WorkEntry {
        std::unique_ptr<C2Work> mWork;
        uint32_t mDrainMode = NO_DRAIN;
    };

    // Internal struct to keep the information of a specific graphic block.
    struct GraphicBlockInfo {
        enum class State {
            OWNED_BY_COMPONENT,    // Owned by this component.
            OWNED_BY_ACCELERATOR,  // Owned by video decode accelerator.
            OWNED_BY_CLIENT,       // Owned by client.
            OWNER_BY_TUNNELRENDER,
        };

        // The ID of this block used for accelerator.
        int32_t mBlockId = -1;
        // The ID of this block used in block pool. It indicates slot index for bufferqueue-backed
        // block pool, and buffer ID of BufferPoolData for bufferpool block pool.
        uint32_t mPoolId = 0;
        State mState = State::OWNED_BY_COMPONENT;
        // Graphic block buffer allocated from allocator. The graphic block should be owned until
        // it is passed to client.
        std::shared_ptr<C2GraphicBlock> mGraphicBlock;
        // HAL pixel format used while importing to VDA.
        HalPixelFormat mPixelFormat;
        // The dmabuf fds dupped from graphic block for importing to VDA.
        std::vector<::base::ScopedFD> mHandles;
        int32_t mFd;
        bool mFdHaveSet;
        bool mBind;
        bool mNeedRealloc;
        // VideoFramePlane information for importing to VDA.
        std::vector<VideoFramePlane> mPlanes;
    };

    struct VideoFormat {
        HalPixelFormat mPixelFormat = HalPixelFormat::UNKNOWN;
        uint32_t mMinNumBuffers = 0;
        media::Size mCodedSize;
        media::Rect mVisibleRect;

        VideoFormat() {}
        VideoFormat(HalPixelFormat pixelFormat, uint32_t minNumBuffers, media::Size codedSize,
                    media::Rect visibleRect);
    };

    // Internal struct for the information of output buffer returned from the accelerator.
    struct OutputBufferInfo {
        int32_t mBitstreamId;
        int32_t mBlockId;
        int64_t mMediaTimeUs;
        int32_t flags;
    };

    // These tasks should be run on the component thread |mThread|.
    void onDestroy();
    void onStart(media::VideoCodecProfile profile, ::base::WaitableEvent* done);
    void onQueueWork(std::unique_ptr<C2Work> work);
    void onDequeueWork();
    void onInputBufferDone(int32_t bitstreamId);
    void onOutputBufferDone(int32_t pictureBufferId, int64_t bitstreamId, int32_t flags);
    void onDrain(uint32_t drainMode);
    void onDrainDone();
    void onStop(::base::WaitableEvent* done);
    void onFlushOrStopDone();
    void onFlushDone();
    void onStopDone();
    void onOutputFormatChanged(std::unique_ptr<VideoFormat> format);
    void onVisibleRectChanged(const media::Rect& cropRect);
    void onOutputBufferReturned(std::shared_ptr<C2GraphicBlock> block, uint32_t poolId,uint32_t blockId);
    void onNewBlockBufferFetched(std::shared_ptr<C2GraphicBlock> block, uint32_t poolId,uint32_t blockId);
    void onReportError(c2_status_t error);

    // Send input buffer to accelerator with specified bitstream id.
    void sendInputBufferToAccelerator(const C2ConstLinearBlock& input, int32_t bitstreamId,  uint64_t timestamp,int32_t flags,uint8_t *hdrbuf = nullptr,uint32_t hdrlen = 0);
    // Send output buffer to accelerator. If |passToAccelerator|, change the ownership to
    // OWNED_BY_ACCELERATOR of this buffer.
    void sendOutputBufferToAccelerator(GraphicBlockInfo* info, bool passToAccelerator);
    // Set crop rectangle infomation to output format.
    void setOutputFormatCrop(const media::Rect& cropRect);
    // Helper function to get the specified GraphicBlockInfo object by its id.
    GraphicBlockInfo* getGraphicBlockById(int32_t blockId);
    // Helper function to get the specified GraphicBlockInfo object by its pool id.
    GraphicBlockInfo* getGraphicBlockByBlockId(uint32_t poolId,uint32_t blockId);
    GraphicBlockInfo* getGraphicBlockByFd(int32_t fd);
    std::deque<C2VDAComponent::OutputBufferInfo>::iterator findPendingBuffersToWorkByTime(int64_t timeus);
    bool erasePendingBuffersToWorkByTime(int64_t timeus);

    //get first unbind graphicblock
    GraphicBlockInfo* getUnbindGraphicBlock();
    // Helper function to find the work iterator in |mPendingWorks| by bitstream id.
    std::deque<std::unique_ptr<C2Work>>::iterator findPendingWorkByBitstreamId(int32_t bitstreamId);
    std::deque<std::unique_ptr<C2Work>>::iterator findPendingWorkByMediaTime(int64_t mediaTime);
    // Helper function to get the specified work in |mPendingWorks| by bitstream id.
    C2Work* getPendingWorkByBitstreamId(int32_t bitstreamId);
    C2Work* getPendingWorkByMediaTime(int64_t mediaTime);
    // Try to apply the output format change.
    void tryChangeOutputFormat();
    //
    c2_status_t allocNonTunnelBuffers(const media::Size& size, uint32_t pixelFormat);
    // Allocate output buffers (graphic blocks) from block pool.
    c2_status_t allocateBuffersFromBlockPool(const media::Size& size, uint32_t pixelFormat);
    // Append allocated buffer (graphic block) to |mGraphicBlocks|.
    void appendOutputBuffer(std::shared_ptr<C2GraphicBlock> block, uint32_t poolId,uint32_t blockId, bool bind);
    // Append allocated buffer (graphic block) to |mGraphicBlocks| in secure mode.
    void appendSecureOutputBuffer(std::shared_ptr<C2GraphicBlock> block, uint32_t poolId);
    // Parse coded color aspects from bitstream and configs parameter if applicable.
    bool parseCodedColorAspects(const C2ConstLinearBlock& input);
    // Update color aspects for current output buffer.
    c2_status_t updateColorAspects();
    //update hdr static info
    c2_status_t updateHDRStaticInfo();
    //update hdr10 plus info
    void updateHDR10PlusInfo();
    // Dequeue |mPendingBuffersToWork| to put output buffer to corresponding work and report if
    // finished as many as possible. If |dropIfUnavailable|, drop all pending existing frames
    // without blocking.
    c2_status_t sendOutputBufferToWorkIfAny(bool dropIfUnavailable);
    // Update |mUndequeuedBlockIds| FIFO by pushing |blockId|.
    void updateUndequeuedBlockIds(int32_t blockId);
    void onCheckVideoDecReconfig();

    // Specific to VP8/VP9, since for no-show frame cases VDA will not call PictureReady to return
    // output buffer which the corresponding work is waiting for, this function detects these works
    // by comparing timestamps. If there are works with no-show frame, call reportWorkIfFinished()
    // to report to listener if finished.
    void detectNoShowFrameWorksAndReportIfFinished(const C2WorkOrdinalStruct& currOrdinal);
    void reportWorkForNoShowFrames();
    // Check if the corresponding work is finished by |bitstreamId|. If yes, make onWorkDone call to
    // listener and erase the work from |mPendingWorks|.
    void reportWorkIfFinished(int32_t bitstreamId, int32_t flags, bool isEmptyWork = false);
    // Make onWorkDone call to listener for reporting EOS work in |mPendingWorks|.
    c2_status_t reportEOSWork();
    // Abandon all works in |mPendingWorks| and |mAbandonedWorks|.
    void reportAbandonedWorks();
    // Make onError call to listener for reporting errors.
    void reportError(c2_status_t error);
    // Helper function to determine if the work indicates no-show output frame.
    bool isNoShowFrameWork(const C2Work& work, const C2WorkOrdinalStruct& currOrdinal) const;
    // Helper function to determine if the work is finished.
    bool isWorkDone(const C2Work* work) const;

    void resetInputAndOutputBufInfo(void);

    bool isNonTunnelMode() const;
    bool isTunnelMode() const;
    bool isTunnerPassthroughMode() const;
    void onAndroidVideoPeek();

    C2Work* cloneWork(C2Work* ori);
    void cloneWork(C2Work* ori, C2Work* out);
    void sendClonedWork(C2Work* work, int32_t flags);
    void reportEmptyWork(int32_t bitstreamId, int32_t flags);

    // Start dequeue thread, return true on success. If |resetBuffersInClient|, reset the counter
    // |mBuffersInClient| on start.
    bool startDequeueThread(const media::Size& size, uint32_t pixelFormat, bool resetBuffersInClient);
    // Stop dequeue thread.
    void stopDequeueThread();
    // The rountine task running on dequeue thread.
    void dequeueThreadLoop(const media::Size& size, uint32_t pixelFormat);

    //display all graphic block information.
    void displayGraphicBlockInfo();
    void getCurrentProcessFdInfo();
    //convert codec profiel to mime
    const char* VideoCodecProfileToMime(media::VideoCodecProfile profile);
    c2_status_t videoResolutionChange();
    bool getVideoResolutionChanged();
    int getDefaultMaxBufNum(InputCodec videotype);
    c2_status_t reallocateBuffersForUsageChanged(const media::Size& size, uint32_t pixelFormat);
    void resetBlockPoolUtil();

    //convert graphicblock state.
    const char* GraphicBlockState(GraphicBlockInfo::State state);
    //get the delay time of fetch block
    int32_t getFetchGraphicBlockDelayTimeUs(c2_status_t err);
    static std::atomic<int32_t> sConcurrentInstances;
    static std::atomic<int32_t> sConcurrentInstanceSecures;

    // The pointer of component interface implementation.
    std::shared_ptr<IntfImpl> mIntfImpl;
    // The pointer of component interface.
    const std::shared_ptr<C2ComponentInterface> mIntf;
    // The pointer of component listener.
    std::shared_ptr<Listener> mListener;

    // The main component thread.
    ::base::Thread mThread;
    // The task runner on component thread.
    scoped_refptr<::base::SingleThreadTaskRunner> mTaskRunner;

    // The dequeue buffer loop thread.
    ::base::Thread mDequeueThread;
    // The stop signal for dequeue loop which should be atomic (toggled by main thread).
    std::atomic<bool> mDequeueLoopStop;
    // The run signal for dequeue loop which should be atomic (toggled by main thread).
    std::atomic<bool> mDequeueLoopRunning;

    // The count of buffers owned by client which should be atomic.
    std::atomic<uint32_t> mBuffersInClient;

    // The following members should be utilized on component thread |mThread|.

    // The initialization result retrieved from VDA.
    VideoDecodeAcceleratorAdaptor::Result mVDAInitResult;
    // The pointer of VideoDecodeAcceleratorAdaptor.
    std::unique_ptr<VideoDecodeAcceleratorAdaptor> mVDAAdaptor;
    // The done event pointer of stop procedure. It should be restored in onStop() and signaled in
    // onStopDone().
    ::base::WaitableEvent* mStopDoneEvent;
    // The state machine on component thread.
    ComponentState mComponentState;
    // The indicator of draining with EOS. This should be always set along with component going to
    // DRAINING state, and will be unset either after reportEOSWork() (EOS is outputted), or
    // reportAbandonedWorks() (drain is cancelled and works are abandoned).
    bool mPendingOutputEOS;
    // The vector of storing allocated output graphic block information.
    std::vector<GraphicBlockInfo> mGraphicBlocks;
    // The work queue. Works are queued along with drain mode from component API queue_nb and
    // dequeued by the decode process of component.
    std::queue<WorkEntry> mQueue;
    // Store all pending works. The dequeued works are placed here until they are finished and then
    // sent out by onWorkDone call to listener.
    // TODO: maybe use priority_queue instead.
    std::deque<std::unique_ptr<C2Work>> mPendingWorks;
    // Store all abandoned works. When component gets flushed/stopped, remaining works in queue are
    // dumped here and sent out by onWorkDone call to listener after flush/stop is finished.
    std::vector<std::unique_ptr<C2Work>> mAbandonedWorks;
    // Store the visible rect provided from VDA. If this is changed, component should issue a
    // visible size change event.
    media::Rect mRequestedVisibleRect;
    // The current output format.
    VideoFormat mOutputFormat;
    // The last output format.
    VideoFormat mLastOutputFormat;
    // The pending output format. We need to wait until all buffers are returned back to apply the
    // format change.
    std::unique_ptr<VideoFormat> mPendingOutputFormat;
    // The color aspects parameter for current decoded output buffers.
    std::shared_ptr<C2StreamColorAspectsInfo::output> mCurrentColorAspects;

    // The record of bitstream and block ID of pending output buffers returned from accelerator.
    std::deque<OutputBufferInfo> mPendingBuffersToWork;
    // A FIFO queue to record the block IDs which are currently undequequed for display. The size
    // of this queue will be equal to the minimum number of undequeued buffers.
    std::deque<int32_t> mUndequeuedBlockIds;
    // The error state indicator which sets to true when an error is occured.
    bool mHasError = false;

    // The indicator of enable dump current process information.
    bool mFdInfoDebugEnable;

    // The indicator of whether component is in secure mode.
    bool mSecureMode;

    // The indicator of whether component is in DolbyVision stream.
    bool mIsDolbyVision;

    // The following members should be utilized on parent thread.

    // The input codec profile which is configured in component interface.
    media::VideoCodecProfile mCodecProfile;
    // The state machine on parent thread which should be atomic.
    std::atomic<State> mState;
    // The mutex lock to synchronize start/stop/reset/release calls.
    std::mutex mStartStopLock;

    // The WeakPtrFactory for getting weak pointer of this.
    ::base::WeakPtrFactory<C2VDAComponent> mWeakThisFactory;

    typedef enum {
        C2_RESOLUTION_CHANGE_NONE,
        C2_RESOLUTION_CHANGEING = 1,
        C2_RESOLUTION_CHANGED = 2,
    } c2_resch_stat;
    enum {
        C2_INTERLACED_TYPE_NONE   = 0x00000000,
        C2_INTERLACED_TYPE_1FIELD = 0x00000000,//one packet contains one field
        C2_INTERLACED_TYPE_2FIELD = 0x00000001,//one packet contains two fields
        C2_INTERLACED_TYPE_SETUP  = 0x00000002,
    };
    enum {
        C2_SYNC_TYPE_NON_TUNNEL = 1,
        C2_SYNC_TYPE_TUNNEL = 2,
        C2_SYNC_TYPE_PASSTHROUTH = 4,
    };
    std::shared_ptr<C2StreamHdrStaticInfo::output> mCurrentHdrStaticInfo;
    std::shared_ptr<C2StreamHdr10PlusInfo::output> mCurrentHdr10PlusInfo;
    std::shared_ptr<C2StreamPictureSizeInfo::output> mCurrentSize;
    std::shared_ptr<C2PortActualDelayTuning::output> mOutputDelay;
    // init param
    mediahal_cfg_parms mConfigParam;
    FILE* mDumpYuvFp;
    static uint32_t mDumpFileCnt;

    std::shared_ptr<VideoDecWraper> mVideoDecWraper;
    std::shared_ptr<MetaDataUtil> mMetaDataUtil;
    std::shared_ptr<C2VDABlockPoolUtil> mBlockPoolUtil;
    std::shared_ptr<TunnelModeHelper> mTunnelHelper;
    //std::shared_ptr<TunnelBufferUtil> mTunnelBufferUtil;

    bool mUseBufferQueue; /*surface use buffer queue */
    bool mBufferFirstAllocated;
    bool mPictureSizeChanged;
    c2_resch_stat mResChStat;
    bool mSurfaceUsageGeted;
    bool mVDAComponentStopDone;
    bool mCanQueueOutBuffer;
    int32_t mOutBufferCount;
    bool mHDR10PlusMeteDataNeedCheck;
    int64_t mInputWorkCount;
    int32_t mInputCSDWorkCount;
    int64_t mOutputWorkCount;
    int32_t mSyncId;
    int64_t mSyncType;
    passthroughInitParams mTunerPassthroughparams;
    TunerPassthroughWrapper *mTunerPassthrough;

    C2ReadView mDefaultDummyReadView;
    std::shared_ptr<C2GraphicBlock> mPendingGraphicBlockBuffer;
    uint32_t mPendingGraphicBlockBufferId;
    uint64_t mLastFlushTimeMs;
    std::vector<int32_t> mNoShowFrameBitstreamIds;
    uint32_t mInterlacedType;
    int64_t mFirstInputTimestamp;
    int32_t mLastOutputBitstreamId;
    int32_t mLastFinishedBitstreamId;
    bool    mNeedFinishFirstWork4Interlaced;
    OutputBufferInfo * mOutPutInfo4WorkIncomplete;
    bool mHasQueuedWork;
    C2Work  mLastOutputC2Work;
    C2Work  mLastFinishedC2Work;

    uint64_t mDefaultRetryBlockTimeOutMs;
    Mutex mFlushDoneLock;
    Condition mFlushDoneCond;

    DISALLOW_COPY_AND_ASSIGN(C2VDAComponent);
};

}  // namespace android

#endif  // ANDROID_C2_VDA_COMPONENT_H
