// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_C2_VDA_COMPONENT_H
#define ANDROID_C2_VDA_COMPONENT_H

//#include <C2VDACommon.h>
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

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <queue>
#include <unordered_map>

#include <AmVideoDecBase.h>
#include <VideoDecWraper.h>
#include <VideoTunnelRendererWraper.h>

namespace android {

class C2VDAComponent : public C2Component,
                       public VideoDecWraper::VideoDecWraperCallback,
                       public std::enable_shared_from_this<C2VDAComponent> {
public:
    class IntfImpl : public C2InterfaceHelper {
    public:
        IntfImpl(C2String name, const std::shared_ptr<C2ReflectorHelper>& helper);

        c2_status_t config(
                const std::vector<C2Param*> &params, c2_blocking_t mayBlock,
                std::vector<std::unique_ptr<C2SettingResult>>* const failures,
                bool updateParams = true,
                std::vector<std::shared_ptr<C2Param>> *changes = nullptr);

        // interfaces for C2VDAComponent
        c2_status_t status() const { return mInitStatus; }
        media::VideoCodecProfile getCodecProfile() const { return mCodecProfile; }
        C2BlockPool::local_id_t getBlockPoolId() const { return mOutputBlockPoolIds->m.values[0]; }
        InputCodec getInputCodec() const { return mInputCodec; }
        void setComponent(C2VDAComponent* comp) {mComponent = comp;}
        std::shared_ptr<C2StreamColorAspectsInfo::output> getColorAspects() {
            return this->mColorAspects;
        }
    private:
        // Configurable parameter setters.
        static C2R ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::input>& info);

        static C2R SizeSetter(bool mayBlock, C2P<C2StreamPictureSizeInfo::output>& videoSize);

        template <typename T>
        static C2R DefaultColorAspectsSetter(bool mayBlock, C2P<T>& def);

        static C2R MergedColorAspectsSetter(bool mayBlock,
                                            C2P<C2StreamColorAspectsInfo::output>& merged,
                                            const C2P<C2StreamColorAspectsTuning::output>& def,
                                            const C2P<C2StreamColorAspectsInfo::input>& coded);
        static C2R Hdr10PlusInfoInputSetter(bool mayBlock, C2P<C2StreamHdr10PlusInfo::input> &me);
        static C2R Hdr10PlusInfoOutputSetter(bool mayBlock, C2P<C2StreamHdr10PlusInfo::output> &me);
        static C2R HdrStaticInfoSetter(bool mayBlock, C2P<C2StreamHdrStaticInfo::output> &me);
        static C2R LowLatencyModeSetter(bool mayBlock, C2P<C2GlobalLowLatencyModeTuning> &me);
        static C2R OutSurfaceAllocatorIdSetter(bool mayBlock, C2P<C2PortSurfaceAllocatorTuning::output> &me);
        static C2R TunnelModeOutSetter(bool mayBlock, C2P<C2PortTunneledModeTuning::output> &me);
        static C2R TunnelHandleSetter(bool mayBlock, C2P<C2PortTunnelHandleTuning::output> &me);
        static C2R TunnelSystemTimeSetter(bool mayBlock, C2P<C2PortTunnelSystemTime::output> &me);

        // The kind of the component; should be C2Component::KIND_ENCODER.
          std::shared_ptr<C2ComponentKindSetting> mKind;
        // The input format kind; should be C2FormatCompressed.
        std::shared_ptr<C2StreamBufferTypeSetting::input> mInputFormat;
        // The output format kind; should be C2FormatVideo.
        std::shared_ptr<C2StreamBufferTypeSetting::output> mOutputFormat;
        // The MIME type of input port.
        std::shared_ptr<C2PortMediaTypeSetting::input> mInputMediaType;
        // The MIME type of output port; should be MEDIA_MIMETYPE_VIDEO_RAW.
        std::shared_ptr<C2PortMediaTypeSetting::output> mOutputMediaType;
        // The input codec profile and level. For now configuring this parameter is useless since
        // the component always uses fixed codec profile to initialize accelerator. It is only used
        // for the client to query supported profile and level values.
        // TODO: use configured profile/level to initialize accelerator.
        std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
        // Decoded video size for output.
        std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
        // Maximum size of one input buffer.
        std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
        // The suggested usage of input buffer allocator ID.
        std::shared_ptr<C2PortAllocatorsTuning::input> mInputAllocatorIds;
        // The suggested usage of output buffer allocator ID.
        std::shared_ptr<C2PortAllocatorsTuning::output> mOutputAllocatorIds;
        // The suggested usage of output buffer allocator ID with surface.
        std::shared_ptr<C2PortSurfaceAllocatorTuning::output> mOutputSurfaceAllocatorId;
        // Compnent uses this ID to fetch corresponding output block pool from platform.
        std::shared_ptr<C2PortBlockPoolsTuning::output> mOutputBlockPoolIds;
        // The color aspects parsed from input bitstream. This parameter should be configured by
        // component while decoding.
        std::shared_ptr<C2StreamColorAspectsInfo::input> mCodedColorAspects;
        // The default color aspects specified by requested output format. This parameter should be
        // configured by client.
        std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
        // The combined color aspects by |mCodedColorAspects| and |mDefaultColorAspects|, and the
        // former has higher priority. This parameter is used for component to provide color aspects
        // as C2Info in decoded output buffers.
        std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
        //hdr
        std::shared_ptr<C2StreamHdrStaticInfo::output> mHdrStaticInfo;
        std::shared_ptr<C2StreamHdr10PlusInfo::input> mHdr10PlusInfoInput;
        std::shared_ptr<C2StreamHdr10PlusInfo::output> mHdr10PlusInfoOutput;
        std::shared_ptr<C2PortActualDelayTuning::input> mActualInputDelay;

        //std::shared_ptr<C2PortActualDelayTuning::input> mActualInputDelay;
        std::shared_ptr<C2PortActualDelayTuning::output> mActualOutputDelay;
        //std::shared_ptr<C2ActualPipelineDelayTuning> mActualPipelineDelay
        //tunnel mode
        std::shared_ptr<C2PortTunneledModeTuning::output> mTunnelModeOutput;
        std::shared_ptr<C2PortTunnelHandleTuning::output> mTunnelHandleOutput;
        std::shared_ptr<C2PortTunnelSystemTime::output> mTunnelSystemTimeOut;

        std::shared_ptr<C2SecureModeTuning> mSecureBufferMode;
        std::shared_ptr<C2GlobalLowLatencyModeTuning> mLowLatencyMode;

        c2_status_t mInitStatus;
        media::VideoCodecProfile mCodecProfile;
        InputCodec mInputCodec;
        C2VDAComponent *mComponent;
        friend C2VDAComponent;
    };

    static std::shared_ptr<C2Component> create(const std::string& name, c2_node_id_t id,
                                               const std::shared_ptr<C2ReflectorHelper>& helper,
                                               C2ComponentFactory::ComponentDeleter deleter);

    C2VDAComponent(C2String name, c2_node_id_t id,
                   const std::shared_ptr<C2ReflectorHelper>& helper);
    virtual ~C2VDAComponent() override;

    class MetaDataUtil;

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
            uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    virtual void UpdateDecInfo(const uint8_t* info, uint32_t isize);
    virtual void NotifyEndOfBitstreamBuffer(int32_t bitstream_buffer_id);
    virtual void NotifyFlushDone();
    virtual void NotifyResetDone();
    virtual void NotifyError(int error);
    virtual void NotifyEvent(uint32_t event, void *param, uint32_t paramsize);

    //tunnel mode implement
    void onConfigureTunnelMode();
    static int fillVideoFrameCallback2(void* obj, void* args);
    int postFillVideoFrameTunnelMode2(int medafd, bool rendered);
    void onFillVideoFrameTunnelMode2(int medafd, bool rendered);
    static int notifyTunnelRenderTimeCallback(void* obj, void* args);
    int postNotifyRenderTimeTunnelMode(struct VideoTunnelRendererWraper::renderTime* rendertime);
    void onNotifyRenderTimeTunnelMode(struct VideoTunnelRendererWraper::renderTime rendertime);
    c2_status_t sendVideoFrameToVideoTunnel(int32_t pictureBufferId, int64_t bitstreamId);
    c2_status_t sendOutputBufferToWorkTunnel(struct VideoTunnelRendererWraper::renderTime* rendertime);

    //for out use
    IntfImpl* GetIntfImpl() {
       return mIntfImpl.get();
    }

    static uint32_t mInstanceNum;
    static uint32_t mInstanceID;
    uint32_t mCurInstanceID;

private:
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
    };

    // These tasks should be run on the component thread |mThread|.
    void onDestroy();
    void onStart(media::VideoCodecProfile profile, ::base::WaitableEvent* done);
    void onQueueWork(std::unique_ptr<C2Work> work);
    void onDequeueWork();
    void onInputBufferDone(int32_t bitstreamId);
    void onOutputBufferDone(int32_t pictureBufferId, int64_t bitstreamId);
    void onDrain(uint32_t drainMode);
    void onDrainDone();
    void onFlush();
    void onStop(::base::WaitableEvent* done);
    void onResetDone();
    void onFlushDone();
    void onStopDone();
    void onOutputFormatChanged(std::unique_ptr<VideoFormat> format);
    void onVisibleRectChanged(const media::Rect& cropRect);
    void onOutputBufferReturned(std::shared_ptr<C2GraphicBlock> block, uint32_t poolId);

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
    GraphicBlockInfo* getGraphicBlockByPoolId(uint32_t poolId);
    GraphicBlockInfo* getGraphicBlockByFd(int32_t fd);
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
    // Allocate output buffers (graphic blocks) from block allocator.
    c2_status_t allocateBuffersFromBlockAllocator(const media::Size& size, uint32_t pixelFormat);
    // Append allocated buffer (graphic block) to |mGraphicBlocks|.
    void appendOutputBuffer(std::shared_ptr<C2GraphicBlock> block, uint32_t poolId, bool bind);
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
    void checkVideoDecReconfig();

    // Specific to VP8/VP9, since for no-show frame cases VDA will not call PictureReady to return
    // output buffer which the corresponding work is waiting for, this function detects these works
    // by comparing timestamps. If there are works with no-show frame, call reportWorkIfFinished()
    // to report to listener if finished.
    void detectNoShowFrameWorksAndReportIfFinished(const C2WorkOrdinalStruct& currOrdinal);
    void reportWorkForNoShowFrames();
    // Check if the corresponding work is finished by |bitstreamId|. If yes, make onWorkDone call to
    // listener and erase the work from |mPendingWorks|.
    void reportWorkIfFinished(int32_t bitstreamId);
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

    C2Work* cloneWork(C2Work* ori);
    void sendClonedWork(C2Work* work);

    // Start dequeue thread, return true on success. If |resetBuffersInClient|, reset the counter
    // |mBuffersInClient| on start.
    bool startDequeueThread(const media::Size& size, uint32_t pixelFormat,
                            std::shared_ptr<C2BlockPool> blockPool, bool resetBuffersInClient);
    // Stop dequeue thread.
    void stopDequeueThread();
    // The rountine task running on dequeue thread.
    void dequeueThreadLoop(const media::Size& size, uint32_t pixelFormat,
                           std::shared_ptr<C2BlockPool> blockPool);
    //convert codec profiel to mime
    const char* VideoCodecProfileToMime(media::VideoCodecProfile profile);
    c2_status_t videoResolutionChange();
    bool getVideoResolutionChanged();
    int getDefaultMaxBufNum(InputCodec videotype);

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

    // The indicator of whether component is in secure mode.
    bool mSecureMode;

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
        C2_INTERLACED_TYPE_1FIELD = 0x00000000,
        C2_INTERLACED_TYPE_2FIELD = 0x00000001,
        C2_INTERLACED_TYPE_SETUP  = 0x00000002,
    };
    std::shared_ptr<C2StreamHdrStaticInfo::output> mCurrentHdrStaticInfo;
    std::shared_ptr<C2StreamHdr10PlusInfo::output> mCurrentHdr10PlusInfo;
    std::shared_ptr<C2StreamPictureSizeInfo::output> mCurrentSize;
    std::shared_ptr<C2PortActualDelayTuning::output> mOutputDelay;
    // init param
    mediahal_cfg_parms mConfigParam;
    FILE* mDumpYuvFp;
    static uint32_t mDumpFileCnt;
    VideoDecWraper* mVideoDecWraper;
    VideoTunnelRendererWraper* mVideoTunnelRenderer;
    std::shared_ptr<MetaDataUtil> mMetaDataUtil;
    int32_t mTunnelId;
    native_handle_t* mTunnelHandle;
    bool mUseBufferQueue; /*surface use buffer queue */
    bool mBufferFirstAllocated;
    bool mPictureSizeChanged;
    std::shared_ptr<C2BlockPool> mBlockPool;
    c2_resch_stat mResChStat;
    bool mSurfaceUsageGeted;
    bool mVDAComponentStopDone;
    bool mIsTunnelMode;
    std::vector<struct fillVideoFrame2> mFillVideoFrameQueue;
    bool mCanQueueOutBuffer;
    std::vector<int64_t> mTunnelAbandonMediaTimeQueue;
    int32_t mOutBufferCount;
    bool mHDR10PlusMeteDataNeedCheck;

    C2ReadView mDefaultDummyReadView;
    std::shared_ptr<C2GraphicBlock> mPendingGraphicBlockBuffer;
    uint32_t mPendingGraphicBlockBufferId;
    uint64_t mLastFlushTimeMs;
    std::vector<int32_t> mNoShowFrameBitstreamIds;
    uint32_t mInterlacedType;
    bool mInterlacedFirstField;
    int64_t mFirstInputTimestamp;

    DISALLOW_COPY_AND_ASSIGN(C2VDAComponent);
};

}  // namespace android

#endif  // ANDROID_C2_VDA_COMPONENT_H
