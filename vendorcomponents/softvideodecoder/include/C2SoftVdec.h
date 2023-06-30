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

#ifndef C2_SOFT_VDEC_COMPONENT_H_
#define C2_SOFT_VDEC_COMPONENT_H_

#include <sys/time.h>
#include <inttypes.h>
#include <atomic>
#include <C2Component.h>
#include <C2ComponentFactory.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>

#include <C2VendorSoftVideoSupport.h>
#include <C2SoftVdecComponent.h>


class AmVideoCodec;
namespace android {

#define ALIGN16(x)                      ((((x) + 15) >> 4) << 4)
#define ALIGN128(x)                     ((((x) + 127) >> 7) << 7)
#define MIN(a, b)                       (((a) < (b)) ? (a) : (b))

class C2SoftVdec : public C2SoftVdecComponent {
public:
    class IntfImpl;
    static std::shared_ptr<C2Component> create(const std::string& name, c2_node_id_t id,
                                               const std::shared_ptr<IntfImpl> &intfImpl);
    C2SoftVdec(C2String name, c2_node_id_t id,
                               const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2SoftVdec();
    // For FFmpeg decoder
    typedef struct VIDEO_FRAME_WRAPPER {
        #define NUM_DATA_POINTERS 8
        uint8_t * data[NUM_DATA_POINTERS];
        int32_t linesize[NUM_DATA_POINTERS];
        int32_t width;
        int32_t height;
        int32_t format;
        int64_t pts;
    }VIDEO_FRAME_WRAPPER_T;

    typedef struct VIDEO_INFO {
        uint8_t *extra_data;
        int32_t extra_data_size;
        int32_t width;
        int32_t height;
    }VIDEO_INFO_T;
    typedef int (*ffmpeg_video_decoder_init_fn)(const char *codeMime, VIDEO_INFO_T *vinfo , AmVideoCodec **mCodec);
    typedef int (*ffmpeg_video_decoder_process_fn)(uint8_t *input_buffer,
                               int input_size,
                               VIDEO_FRAME_WRAPPER_T *outPic,
                               AmVideoCodec *mCodec);
    typedef int (*ffmpeg_video_decoder_free_frame_fn)(AmVideoCodec *mCodec);
    typedef int (*ffmpeg_video_decoder_close_fn)(AmVideoCodec *mCodec);

private:
    /**
     * Initialize internal states of the component according to the config set
     * in the interface.
     *
     * This method is called during start(), but only at the first invocation or
     * after reset().
     */
    c2_status_t onInit() override;

    /**
     * Stop the component.
     */
   c2_status_t onStop() override;

    /**
     * Reset the component.
     */
    void onReset() override;

    /**
     * Release the component.
     */
    void onRelease() override;

    /**
     * Flush the component.
     */
    c2_status_t onFlush_sm() override;

    /**
     * Process the given work and finish pending work using finish().
     *
     * \param[in,out]   work    the work to process
     * \param[in]       pool    the pool to use for allocating output blocks.
     */
    void process(
            const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2BlockPool> &pool) override;

    /**
     * Drain the component and finish pending work using finish().
     *
     * \param[in]   drainMode   mode of drain.
     * \param[in]   pool        the pool to use for allocating output blocks.
     *
     * \retval C2_OK            The component has drained all pending output
     *                          work.
     * \retval C2_OMITTED       Unsupported mode (e.g. DRAIN_CHAIN)
     */
    c2_status_t drain(
            uint32_t drainMode,
            const std::shared_ptr<C2BlockPool> &pool) override;

    status_t createDecoder();
    void getVersion();
    status_t initDecoder();
    c2_status_t ensureDecoderState(const std::shared_ptr<C2BlockPool> &pool);
    void finishWork(uint64_t index, const std::unique_ptr<C2Work> &work);
    status_t setFlushMode();
    c2_status_t drainInternal(
            uint32_t drainMode,
            const std::shared_ptr<C2BlockPool> &pool,
            const std::unique_ptr<C2Work> &work);
    status_t resetDecoder();
    void resetPlugin();
    status_t deleteDecoder();



    bool load_ffmpeg_decoder_lib();
    bool unload_ffmpeg_decoder_lib();
    // End

    static constexpr uint32_t NO_DRAIN = ~0u;

    static std::atomic<int32_t> sConcurrentInstances;

    // The pointer of component interface implementation.
    std::shared_ptr<IntfImpl> mIntfImpl;
    std::shared_ptr<C2GraphicBlock> mOutBlock;
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    // Store all pending works. The dequeued works are placed here until they are finished and then
    // sent out by onWorkDone call to listener.
    // TODO: maybe use priority_queue instead.
    std::list<uint64_t> mPendingWorkFrameIndexes;

    C2String mDecoderName;
    uint32_t mWidth;
    uint32_t mHeight;
    uint64_t mTotalDropedOutputFrameNum;
    uint64_t mTotalProcessedFrameNum;
    std::atomic_uint64_t mOutIndex;
    bool mSignalledOutputEos;
    bool mSignalledError;
    bool mFirstPictureReviced;

    bool mDecInit;
    VIDEO_FRAME_WRAPPER_T *mPic;
    VIDEO_INFO_T mVideoInfo;

    AmVideoCodec *mCodec;
    uint8_t* mExtraData;

    bool mDumpYuvEnable;
    FILE* mDumpYuvFp;
    static uint32_t mDumpFileCnt;

    // For decode time calculate.
    nsecs_t mTimeStart = 0;
    nsecs_t mTimeEnd = 0;
    nsecs_t mTimeTotal = 0;

    C2_DO_NOT_COPY(C2SoftVdec);
};

}  // namespace android

#endif  // C2_SOFT_VDEC_COMPONENT_H_
