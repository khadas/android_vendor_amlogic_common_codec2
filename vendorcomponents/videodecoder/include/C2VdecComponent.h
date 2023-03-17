// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef C2_VDEC_COMPONENT_H
#define C2_VDEC_COMPONENT_H

#include <VideoDecodeAcceleratorAdaptor.h>

#include <iostream>
#include <sstream>
#include <string>
#include <stdlib.h>

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
#include <C2VdecBlockPoolUtil.h>
#include <C2VendorSupport.h>
#include <AmlMessageBase.h>

namespace android {

#define DECLARE_C2_DEFAULT_UNSTRICT_SETTER(s,n) \
    static C2R n##Setter(bool mayBlock, C2P<s> &me)

class C2VdecComponent : public C2Component,
                       public VideoDecWraper::VideoDecWraperCallback,
                       public std::enable_shared_from_this<C2VdecComponent> {
public:
    class IntfImpl;
    static std::shared_ptr<C2Component> create(const std::string& name, c2_node_id_t id,
                                               const std::shared_ptr<C2ReflectorHelper>& helper,
                                               C2ComponentFactory::ComponentDeleter deleter);

    C2VdecComponent(C2String name, c2_node_id_t id,
                   const std::shared_ptr<C2ReflectorHelper>& helper);
    virtual ~C2VdecComponent() override;

    void  Init(C2String compName);
    class DeviceUtil;
    class TunnelHelper;
    class TunerPassthroughHelper;
    class DebugUtil;
    class DequeueThreadUtil;

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
    virtual void PictureReady(output_buf_param_t* params);
    virtual void UpdateDecInfo(const uint8_t* info, uint32_t isize);
    virtual void NotifyEndOfBitstreamBuffer(int32_t bitstream_buffer_id);
    virtual void NotifyFlushDone();
    virtual void NotifyFlushOrStopDone();
    virtual void NotifyError(int error);
    virtual void NotifyEvent(uint32_t event, void *param, uint32_t paramsize);

    //config
    void onConfigureTunnelMode();
    void onConfigureTunerPassthroughMode();
    void onConfigureTunerPassthroughTrickMode();

    //for out use
    bool isAmDolbyVision() {return mIsDolbyVision;}
    bool isSecureMode() {return mSecureMode;}
    bool isResolutionChanging ();

    std::shared_ptr<IntfImpl> GetIntfImpl() {return mIntfImpl;}
    std::shared_ptr<DeviceUtil> GetDeviceUtil() {return mDeviceUtil;}
    std::shared_ptr<C2VdecBlockPoolUtil> GetBlockPoolUtil() {return mBlockPoolUtil;}
    media::Size GetCurrentVideoSize() {return mOutputFormat.mCodedSize;}
    scoped_refptr<::base::SingleThreadTaskRunner> GetTaskRunner() {return mTaskRunner;}
    std::shared_ptr<VideoDecWraper> getCompVideoDecWraper() {return mVideoDecWraper;}

    //for multi-instance trace
    std::ostringstream TRACE_NAME_IN_PTS;
    std::ostringstream TRACE_NAME_BITSTREAM_ID;
    std::ostringstream TRACE_NAME_FETCH_OUT_BLOCK_ID;
    std::ostringstream TRACE_NAME_OUT_PTS;
    std::ostringstream TRACE_NAME_FINISHED_WORK_PTS;

    static uint32_t mInstanceNum;
    static uint32_t mInstanceID;
    uint32_t mCurInstanceID;

    static const uint32_t kUpdateDurationFramesNumMax = 10;
    int mUpdateDurationUsCount;

    bool IsCompHaveCurrentBlock(uint32_t poolId, uint32_t blockId);
    bool IsCheckStopDequeueTask();
    void onOutputBufferReturned(std::shared_ptr<C2GraphicBlock> block, uint32_t poolId,uint32_t blockId);
    void onNewBlockBufferFetched(std::shared_ptr<C2GraphicBlock> block, uint32_t poolId,uint32_t blockId);

private:
    friend TunnelHelper;
    friend TunerPassthroughHelper;
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
        // This is the initial state until Vdec initialization returns successfully.
        UNINITIALIZED,
        // Vdec initialization returns successfully. Vdec is ready to make progress.
        STARTED,
        // onDrain() is called. Vdec is draining. Component will hold on queueing works until
        // onDrainDone().
        DRAINING,
        // onFlush() is called. Vdec is flushing. State will change to STARTED after onFlushDone().
        FLUSHING,
        // onStop() is called. Vdec is shutting down. State will change to UNINITIALIZED after
        // onStopDone().
        STOPPING,
        //when destructor is called, Vdec is destroying.  state will change to DESTROYED after onDestroy
        DESTROYING,
        //after onDestroy is called, Vdec is destroyed, state will change to DESTROYED
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
        std::shared_ptr<C2StreamHdrDynamicMetadataInfo::input> mHdr10PlusInfo;
    };

    // Internal struct to keep the information of a specific graphic block.
    struct GraphicBlockInfo {
        enum class State {
            OWNED_BY_COMPONENT = 0,    // Owned by this component.
            OWNED_BY_ACCELERATOR,  // Owned by video decode accelerator.
            OWNED_BY_CLIENT,       // Owned by client.
            OWNER_BY_TUNNELRENDER,
            GRAPHIC_BLOCK_OWNER_MAX = OWNER_BY_TUNNELRENDER + 1,
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
        // HAL pixel format used while importing to Vdec.
        HalPixelFormat mPixelFormat;
        // The dmabuf fds duplicated from graphic block for importing to Vdec.
        std::vector<::base::ScopedFD> mHandles;
        int32_t mFd;
        bool mFdHaveSet;
        bool mBind;
        bool mNeedRealloc;
        // VideoFramePlane information for importing to Vdec.
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
        uint64_t mMediaTimeUs;
        int32_t flags;
        bool    mSetOutInfo;
    };

    // These tasks should be run on the component thread |mThread|.
    void onDestroy(::base::WaitableEvent* done);
    void onStart(media::VideoCodecProfile profile, ::base::WaitableEvent* done);
    void onQueueWork(std::unique_ptr<C2Work> work, std::shared_ptr<C2StreamHdrDynamicMetadataInfo::input> info);
    void onDequeueWork();
    void onInputBufferDone(int32_t bitstreamId);
    void onOutputBufferDone(int32_t pictureBufferId, int64_t bitstreamId, int32_t flags, uint64_t timestamp);
    void onDrain(uint32_t drainMode);
    void onDrainDone();
    void onFlush();
    void onStop(::base::WaitableEvent* done);
    void onFlushOrStopDone();
    void onFlushDone();
    void onStopDone();
    void onOutputFormatChanged(std::unique_ptr<VideoFormat> format);
    void onVisibleRectChanged(const media::Rect& cropRect);

    void onReportError(c2_status_t error);

    // Send input buffer to accelerator with specified bitstream id.
    void sendInputBufferToAccelerator(const C2ConstLinearBlock& input, int32_t bitstreamId,  uint64_t timestamp,int32_t flags,uint8_t *hdrbuf = nullptr,uint32_t hdrlen = 0);
    // Send output buffer to accelerator. If |passToAccelerator|, change the ownership to
    // OWNED_BY_ACCELERATOR of this buffer.
    void sendOutputBufferToAccelerator(GraphicBlockInfo* info, bool passToAccelerator);
    // Set crop rectangle information to output format.
    void setOutputFormatCrop(const media::Rect& cropRect);
    // Helper function to get the specified GraphicBlockInfo object by its id.
    GraphicBlockInfo* getGraphicBlockById(int32_t blockId);
    // Helper function to get the specified GraphicBlockInfo object by its pool id.
    GraphicBlockInfo* getGraphicBlockByBlockId(uint32_t poolId,uint32_t blockId);
    GraphicBlockInfo* getGraphicBlockByFd(int32_t fd);
    std::deque<C2VdecComponent::OutputBufferInfo>::iterator findPendingBuffersToWorkByTime(uint64_t timeus);
    bool erasePendingBuffersToWorkByTime(uint64_t timeus);

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
    void sendOutputBufferToWorkIfAnyTask(bool dropIfUnavailable);
    void updateWorkParam(C2Work* work, GraphicBlockInfo* info);
    // Update |mUndequeuedBlockIds| FIFO by pushing |blockId|.
    void updateUndequeuedBlockIds(int32_t blockId);
    void onCheckVideoDecReconfig();

    // Specific to VP8/VP9, since for no-show frame cases Vdec will not call PictureReady to return
    // output buffer which the corresponding work is waiting for, this function detects these works
    // by comparing timestamps. If there are works with no-show frame, call reportWorkIfFinished()
    // to report to listener if finished.
    void detectNoShowFrameWorksAndReportIfFinished(const C2WorkOrdinalStruct& currOrdinal);
    // For the frames that cannot be decoded by the decoder, the work ID is returned through the event,
    // and then directly returned to the framework for discarding.
    void onErrorFrameWorksAndReportIfFinised(int32_t bitstreamId);

    void reportWorkForNoShowFrames();
    // Check if the corresponding work is finished by |bitstreamId|. If yes, make onWorkDone call to
    // listener and erase the work from |mPendingWorks|.
    c2_status_t reportWorkIfFinished(int32_t bitstreamId, int32_t flags, bool isEmptyWork = false);
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
    bool isInputWorkDone(const C2Work* work) const;

    uint32_t getBitDepthByColorAspects();
    void resetInputAndOutputBufInfo(void);

    bool isNonTunnelMode() const;
    bool isTunnelMode() const;
    bool isTunnerPassthroughMode() const;
    void onAndroidVideoPeek();

    C2Work* cloneWork(C2Work* ori);
    void sendClonedWork(C2Work* work, int32_t flags);
    void reportWork(std::unique_ptr<C2Work> work);
    void reportEmptyWork(int32_t bitstreamId, int32_t flags);

    // Start dequeue thread, return true on success. If |resetBuffersInClient|, reset the counter
    // |mBuffersInClient| on start.
    bool startDequeueThread(const media::Size& size, uint32_t pixelFormat, bool resetBuffersInClient);
    // Stop dequeue thread.
    void stopDequeueThread();
    // The routine task running on dequeue thread.
    void dequeueThreadLoop(const media::Size& size, uint32_t pixelFormat);

    //convert codec profile to mime
    const char* VideoCodecProfileToMime(media::VideoCodecProfile profile);
    c2_status_t videoResolutionChange();
    bool getVideoResolutionChanged();
    int getDefaultMaxBufNum(InputCodec videotype);
    c2_status_t reallocateBuffersForUsageChanged(const media::Size& size, uint32_t pixelFormat);
    void resetBlockPoolUtil();

    void onNoOutFrameNotify(int64_t bitstreamId);
    bool isNoOutFrameDone(int64_t bitstreamId, const C2Work* work);
    void onReportNoOutFrameFinished();

    //convert graphicblock state.
    const char* GraphicBlockState(GraphicBlockInfo::State state);
    //get the delay time of fetch block
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
    media::Size mCurrentBlockSize;
    uint32_t mCurrentPixelFormat;

    // The initialization result retrieved from Vdec.
    VideoDecodeAcceleratorAdaptor::Result mVdecInitResult;
    // The pointer of VideoDecodeAcceleratorAdaptor.
    std::unique_ptr<VideoDecodeAcceleratorAdaptor> mVdecAdaptor;
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
    int32_t mGraphicBlockStateCount[(int32_t)GraphicBlockInfo::State::GRAPHIC_BLOCK_OWNER_MAX];
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
    std::list<std::unique_ptr<C2Work>> mFlushPendingWorkList;
    // Store the visible rect provided from Vdec. If this is changed, component should issue a
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
    // A FIFO queue to record the block IDs which are currently undequeued for display. The size
    // of this queue will be equal to the minimum number of undequeued buffers.
    std::deque<int32_t> mUndequeuedBlockIds;
    // The error state indicator which sets to true when an error is occurred.
    bool mHasError = false;

    // The indicator of enable dump current process information.
    bool mFdInfoDebugEnable;

    // The indicator of whether component is in secure mode.
    bool mSecureMode;

    // The indicator of whether component is in DolbyVision stream.
    bool mIsDolbyVision;

    // The input codec profile which is configured in component interface.
    media::VideoCodecProfile mCodecProfile;
    // The state machine on parent thread which should be atomic.
    std::atomic<State> mState;
    // The mutex lock to synchronize start/stop/reset/release calls.
    std::mutex mStartStopLock;

    // The WeakPtrFactory for getting weak pointer of this.
    ::base::WeakPtrFactory<C2VdecComponent> mWeakThisFactory;

    typedef enum {
        C2_RESOLUTION_CHANGE_NONE,
        C2_RESOLUTION_CHANGING = 1,
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
        C2_SYNC_TYPE_PASSTHROUGH = 4,
    };
    std::shared_ptr<C2StreamHdrStaticInfo::output> mCurrentHdrStaticInfo;
    std::shared_ptr<C2StreamHdr10PlusInfo::output> mCurrentHdr10PlusInfo;
    std::shared_ptr<C2StreamPictureSizeInfo::output> mCurrentSize;
    std::shared_ptr<C2PortActualDelayTuning::output> mOutputDelay;
    // init param
    mediahal_cfg_parms mConfigParam;

    std::shared_ptr<VideoDecWraper> mVideoDecWraper;
    std::shared_ptr<DeviceUtil> mDeviceUtil;
    std::shared_ptr<C2VdecBlockPoolUtil> mBlockPoolUtil;
    std::shared_ptr<TunnelHelper> mTunnelHelper;
    std::shared_ptr<TunerPassthroughHelper> mTunerPassthroughHelper;
    std::shared_ptr<DebugUtil> mDebugUtil;
    std::shared_ptr<DequeueThreadUtil> mDequeueThreadUtil;

    bool mUseBufferQueue; /*surface use buffer queue */
    bool mBufferFirstAllocated;
    bool mPictureSizeChanged;
    c2_resch_stat mResChStat;
    bool mSurfaceUsageGot;
    bool mVdecComponentStopDone;
    bool mCanQueueOutBuffer;
    int32_t mOutBufferCount;
    bool mHDR10PlusMeteDataNeedCheck;
    int64_t mInputWorkCount;
    int32_t mInputCSDWorkCount;
    int32_t mInputBufferNum;
    int32_t mInputQueueNum;
    int64_t mOutputWorkCount;
    int64_t mErrorFrameWorkCount;
    int64_t mOutputFinishedWorkCount;
    int32_t mSyncId;
    int64_t mSyncType;
    bool mTunnelUnderflow;

    C2ReadView mDefaultDummyReadView;
    std::shared_ptr<C2GraphicBlock> mPendingGraphicBlockBuffer;
    uint32_t mPendingGraphicBlockBufferId;
    uint64_t mLastFlushTimeMs;
    std::vector<int32_t> mNoShowFrameBitstreamIds;
    uint32_t mInterlacedType;
    int64_t mFirstInputTimestamp;
    int32_t mLastOutputBitstreamId;
    int32_t mLastFinishedBitstreamId;
    bool mHasQueuedWork;
    bool mIsReportEosWork;
    bool mReportEosWork;
    bool mSupport10BitDepth;
    //no correspond outframe work
    std::deque<int64_t> mNoOutFrameWorkQueue;

    uint64_t mDefaultRetryBlockTimeOutMs;
    Mutex mFlushDoneLock;
    Mutex mFlushDoneWorkLock;
    Condition mFlushDoneCond;

    Mutex mResolutionChangingLock;
    bool mResolutionChanging;

    C2Work *mLastOutputReportWork;
    int32_t mPlayerId;
    int32_t mUnstable;
    DISALLOW_COPY_AND_ASSIGN(C2VdecComponent);
};

}  // namespace android

#endif  // C2_VDEC_COMPONENT_H
