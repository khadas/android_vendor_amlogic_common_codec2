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
#define LOG_TAG "C2SoftVdec"

#include <dlfcn.h>
#include <cutils/properties.h>
#include <media/stagefright/foundation/MediaDefs.h>
#include <C2Config.h>
#include <C2PlatformSupport.h>
#include <Codec2BufferUtils.h>
#include <Codec2CommonUtils.h>
#include <Codec2Mapper.h>

#include <C2VendorProperty.h>
#include <C2VendorDebug.h>
#include <C2SoftVdec.h>
#include <C2SoftVdecInterfaceImpl.h>

#define MAX_WORK_PENDING_COUNT (7)

#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

uint32_t android::C2SoftVdec::mDumpFileCnt = 0;

namespace android {

// static
std::atomic<int32_t> C2SoftVdec::sConcurrentInstances = 0;

void *mFFmpegCodecLibHandler = NULL;
C2SoftVdec::ffmpeg_video_decoder_init_fn mFFmpegVideoDecoderInitFunc = NULL;
C2SoftVdec::ffmpeg_video_decoder_process_fn mFFmpegVideoDecoderProcessFunc = NULL;
C2SoftVdec::ffmpeg_video_decoder_free_frame_fn mFFmpegVideoDecoderFreeFrameFunc = NULL;
C2SoftVdec::ffmpeg_video_decoder_close_fn mFFmpegVideoDecoderCloseFunc = NULL;

// static
std::shared_ptr<C2Component> C2SoftVdec::create(
        const std::string& name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl) {
    static const int32_t kMaxConcurrentInstances =
            property_get_int32(C2_PROPERTY_SOFTVDEC_INST_MAX_NUM, 2);
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (kMaxConcurrentInstances >= 0 && sConcurrentInstances.load() >= kMaxConcurrentInstances) {
        ALOGW("Reject to Initialize() due to too many instances: %d", sConcurrentInstances.load());
        return nullptr;
    }
    return std::shared_ptr<C2Component>(new C2SoftVdec(name, id, intfImpl));
}

C2SoftVdec::C2SoftVdec(C2String name, c2_node_id_t id,
                               const std::shared_ptr<IntfImpl> &intfImpl)
        : C2SoftVdecComponent(std::make_shared<SimpleInterface<IntfImpl>>(name.c_str(), id, intfImpl)),
        mIntfImpl(intfImpl),
        mDecoderName(name),
        mWidth(320),
        mHeight(240),
        mTotalDropedOutputFrameNum(0),
        mTotalProcessedFrameNum(0),
        mOutIndex(0u),
        mSignalledOutputEos(false),
        mSignalledError(false),
        mFirstPictureReviced(false),
        mDecInit(false),
        mPic(NULL),
        mCodec(NULL),
        mExtraData(NULL),
        mDumpYuvFp(NULL) {
        sConcurrentInstances.fetch_add(1, std::memory_order_relaxed);
        memset(&mVideoInfo, 0, sizeof(VIDEO_INFO_T));

        CODEC2_LOG(CODEC2_LOG_INFO, "Create %s(%s)", __func__, name.c_str());

        propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
        mDumpYuvEnable = property_get_bool(C2_PROPERTY_SOFTVDEC_DUMP_YUV, false);
        if (mDumpYuvEnable) {
            char pathFile[1024] = { '\0'  };
            sprintf(pathFile, "/data/tmp/codec2_%d.yuv", mDumpFileCnt++);
            mDumpYuvFp = fopen(pathFile, "wb");
            if (mDumpYuvFp) {
                CODEC2_LOG(CODEC2_LOG_INFO, "Open file %s", pathFile);
            } else {
                CODEC2_LOG(CODEC2_LOG_ERR, "Open file %s error:%s", pathFile, strerror(errno));
            }
        }
}

C2SoftVdec::~C2SoftVdec() {
    CODEC2_LOG(CODEC2_LOG_INFO, "%s", __func__);
    mPendingWorkFrameIndexes.clear();
    onRelease();
    if (mExtraData) {
        free(mExtraData);
        mExtraData = NULL;
    }
    if (mDumpYuvFp) {
        fclose(mDumpYuvFp);
    }
    sConcurrentInstances.fetch_sub(1, std::memory_order_relaxed);
    //coverity[Error handling issues]
    CODEC2_LOG(CODEC2_LOG_INFO, "%s done", __func__);
}

c2_status_t C2SoftVdec::onInit() {
    // Update width and height from Config
    CODEC2_LOG(CODEC2_LOG_INFO, "%s", __func__);
    mSize = mIntfImpl->getSize_l();
    if (mSize->width != 0 && mSize->height != 0) {
        mWidth = mSize->width;
        mHeight = mSize->height;
        CODEC2_LOG(CODEC2_LOG_INFO, "Set mWidth=%d, mHeight=%d from CCodecConfig", mWidth, mHeight);
    }

    if ((mDecoderName == "c2.amlogic.vc1.decoder.sw") && (mWidth * mHeight > 1280 * 720)) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Unsupported resolution!");
        return C2_CORRUPTED;
    }

    status_t err = initDecoder();
    return err == OK ? C2_OK : C2_CORRUPTED;
}

c2_status_t C2SoftVdec::onStop() {
    CODEC2_LOG(CODEC2_LOG_INFO, "%s", __func__);
    if (OK != resetDecoder()) {
        return C2_CORRUPTED;
    }
    resetPlugin();
    return C2_OK;
}

void C2SoftVdec::onReset() {
    CODEC2_LOG(CODEC2_LOG_INFO, "%s", __func__);
    onStop();
}

void C2SoftVdec::onRelease() {
   CODEC2_LOG(CODEC2_LOG_INFO, "%s", __func__);
   deleteDecoder();
    if (mOutBlock) {
        mOutBlock.reset();
    }
}

c2_status_t C2SoftVdec::onFlush_sm() {
    CODEC2_LOG(CODEC2_LOG_INFO, "%s", __func__);
    resetDecoder();
    resetPlugin();
    mSignalledOutputEos = false;
    mFirstPictureReviced = false;
    mPendingWorkFrameIndexes.clear();
    return C2_OK;
}

bool C2SoftVdec::load_ffmpeg_decoder_lib(){
    if (mFFmpegCodecLibHandler) {
        return true;
    }

    mFFmpegCodecLibHandler = dlopen("libamffmpegcodec.so", RTLD_NOW);
    if (!mFFmpegCodecLibHandler) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Failed to open ffmpeg decoder lib, %s", dlerror());
        goto Error;
    }

    mFFmpegVideoDecoderInitFunc = (ffmpeg_video_decoder_init_fn)dlsym(mFFmpegCodecLibHandler, "ffmpeg_video_decoder_init");
    if (mFFmpegVideoDecoderInitFunc == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Find lib err:,%s", dlerror());
        goto Error;
    }

    mFFmpegVideoDecoderProcessFunc = (ffmpeg_video_decoder_process_fn)dlsym(mFFmpegCodecLibHandler, "ffmpeg_video_decoder_process");
    if (mFFmpegVideoDecoderProcessFunc == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Find lib err,%s", dlerror());
        goto Error;
    }

    mFFmpegVideoDecoderFreeFrameFunc = (ffmpeg_video_decoder_free_frame_fn)dlsym(mFFmpegCodecLibHandler, "ffmpeg_video_decoder_free_frame");
    if (mFFmpegVideoDecoderFreeFrameFunc == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Find lib err,%s", dlerror());
        goto Error;
    }

    mFFmpegVideoDecoderCloseFunc = (ffmpeg_video_decoder_close_fn)dlsym(mFFmpegCodecLibHandler, "ffmpeg_video_decoder_close");
    if (mFFmpegVideoDecoderCloseFunc == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Find lib err:%s", dlerror());
        goto Error;
    }
    return true;

Error:
    unload_ffmpeg_decoder_lib();
    return false;
}

bool C2SoftVdec::unload_ffmpeg_decoder_lib(){
    if (mFFmpegVideoDecoderCloseFunc != NULL)
        mFFmpegVideoDecoderCloseFunc(mCodec);

    mCodec = NULL;
    return true;
}

status_t C2SoftVdec::initDecoder() {
    if (!load_ffmpeg_decoder_lib()) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Load ffmpeg decoder lib failed!");
        return UNKNOWN_ERROR;
    }
    mSignalledError = false;
    return OK;
}

status_t C2SoftVdec::resetDecoder() {
    deleteDecoder();
    mDecInit = false;
    if (initDecoder() != OK) {
        return UNKNOWN_ERROR;
    }
    mSignalledError = false;
    return OK;
}

void C2SoftVdec::resetPlugin() {
    mSignalledOutputEos = false;
    mTimeStart = mTimeEnd = systemTime();
}

status_t C2SoftVdec::deleteDecoder() {
    unload_ffmpeg_decoder_lib();
    return OK;
}

static void fillEmptyWork(const std::unique_ptr<C2Work> &work) {
    uint32_t flags = 0;
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        flags |= C2FrameData::FLAG_END_OF_STREAM;
        CODEC2_LOG(CODEC2_LOG_INFO, "Signalling EOS");
    }
    work->worklets.front()->output.flags = (C2FrameData::flags_t)flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

void C2SoftVdec::finishWork(uint64_t index, const std::unique_ptr<C2Work> &work) {
    std::shared_ptr<C2Buffer> buffer = createGraphicBuffer(std::move(mOutBlock),
                                                           C2Rect(mWidth, mHeight));
    mOutBlock = nullptr;
    {
        IntfImpl::Lock lock = mIntfImpl->lock();
        buffer->setInfo(mIntfImpl->getColorAspects_l());
    }

    class FillWork {
       public:
        FillWork(uint32_t flags, C2WorkOrdinalStruct ordinal,
                 const std::shared_ptr<C2Buffer>& buffer)
            : mFlags(flags), mOrdinal(ordinal), mBuffer(buffer) {}
        ~FillWork() = default;

        void operator()(const std::unique_ptr<C2Work>& work) {
            work->worklets.front()->output.flags = (C2FrameData::flags_t)mFlags;
            work->worklets.front()->output.buffers.clear();
            work->worklets.front()->output.ordinal = mOrdinal;
            work->workletsProcessed = 1u;
            work->result = C2_OK;
            if (mBuffer) {
                work->worklets.front()->output.buffers.push_back(mBuffer);
            }
            CODEC2_LOG(CODEC2_LOG_INFO, "Timestamp = %lld, index = %lld, w/%s buffer",
                  mOrdinal.timestamp.peekll(), mOrdinal.frameIndex.peekll(),
                  mBuffer ? "" : "o");
        }

       private:
        const uint32_t mFlags;
        const C2WorkOrdinalStruct mOrdinal;
        const std::shared_ptr<C2Buffer> mBuffer;
    };

    auto fillWork = [buffer](const std::unique_ptr<C2Work> &work) {
        work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.buffers.push_back(buffer);
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
    };
    if (work && c2_cntr64_t(index) == work->input.ordinal.frameIndex) {
        bool eos = ((work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
        // TODO: Check if cloneAndSend can be avoided by tracking number of frames remaining
        if (eos) {
            mOutIndex = index;
            C2WorkOrdinalStruct outOrdinal = work->input.ordinal;
            cloneAndSend(
                mOutIndex, work,
                FillWork(C2FrameData::FLAG_INCOMPLETE, outOrdinal, buffer));
            buffer.reset();
        } else {
            fillWork(work);
        }
    } else {
        if (work) {
            finish(index, work->input.ordinal.customOrdinal.peeku(), fillWork);
        }
    }
}

c2_status_t C2SoftVdec::ensureDecoderState(const std::shared_ptr<C2BlockPool> &pool) {
    if (!mFFmpegVideoDecoderInitFunc || !mFFmpegVideoDecoderProcessFunc || !mFFmpegVideoDecoderFreeFrameFunc) {
        ALOGE("not supposed to be here, invalid decoder context");
        //return C2_CORRUPTED;
    }
    if (mOutBlock &&
            (mOutBlock->width() != ALIGN16(mWidth) || mOutBlock->height() != mHeight)) {
        mOutBlock.reset();
    }
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Start fetchGraphicBlock, Required (%dx%d)", ALIGN16(mWidth), mHeight);
    if (!mOutBlock) {
        uint32_t format = HAL_PIXEL_FORMAT_YV12;
        C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
        c2_status_t err =
            pool->fetchGraphicBlock(ALIGN16(mWidth), mHeight, format, usage, &mOutBlock);
        if (err != C2_OK) {
            CODEC2_LOG(CODEC2_LOG_ERR, "FetchGraphicBlock for Output failed with status %d", err);
            return err;
        }
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "FetchGraphicBlock done, Provided (%dx%d) Required (%dx%d)",
              mOutBlock->width(), mOutBlock->height(), ALIGN16(mWidth), mHeight);
    }
    return C2_OK;
}

// TODO: can overall error checking be improved?
// TODO: allow configuration of color format and usage for graphic buffers instead
//       of hard coding them to HAL_PIXEL_FORMAT_YV12
// TODO: pass coloraspects information to surface
// TODO: test support for dynamic change in resolution
// TODO: verify if the decoder sent back all frames
void C2SoftVdec::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    // Initialize output work
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.configUpdate.clear();
    work->worklets.front()->output.flags = work->input.flags;

    if (mSignalledError || mSignalledOutputEos) {
        work->result = C2_BAD_VALUE;
        return;
    }

    size_t inSize = 0u;
    uint32_t workIndex = work->input.ordinal.frameIndex.peeku() & 0xFFFFFFFF;
    C2ReadView rView = mDummyReadView;
    if (!work->input.buffers.empty()) {
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inSize = rView.capacity();
        if (inSize && rView.error()) {
            CODEC2_LOG(CODEC2_LOG_ERR, "Read view map failed %d", rView.error());
            work->result = rView.error();
            return;
        }
    }

    uint8_t *inBuffer = const_cast<uint8_t *>(rView.data());
    bool codecConfig = ((work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) !=0);
    bool eos = ((work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
    bool frameHasData = (inSize > 0);
    bool flushPendingWork = (eos && !mPendingWorkFrameIndexes.empty());
    bool hasPicture = false;

    // Config csd data
    if (codecConfig) {
        if (inSize > 0) {
            CODEC2_LOG(CODEC2_LOG_INFO, "Config num:%zu", inSize);
            if (mExtraData != NULL) {
                free(mExtraData);
            }
            mExtraData = (uint8_t *)malloc(inSize);
            if (mExtraData == NULL) {
                work->result = C2_NO_MEMORY;
                return;
            }
            memcpy(mExtraData, inBuffer, inSize);
            mVideoInfo.extra_data = mExtraData;
            mVideoInfo.extra_data_size = inSize;
        }
        CODEC2_LOG(CODEC2_LOG_INFO, "For %s don't input config pkt to ffmpeg", mDecoderName.c_str());
        fillEmptyWork(work);
        return;
    }

    // Loop for resolution changed case.
    while (frameHasData || flushPendingWork) {
        if (C2_OK != ensureDecoderState(pool)) {
            mSignalledError = true;
            work->workletsProcessed = 1u;
            work->result = C2_CORRUPTED;
            return;
        }

        C2GraphicView wView = mOutBlock->map().get();
        if (wView.error()) {
            CODEC2_LOG(CODEC2_LOG_ERR, "Graphic view map failed %d", wView.error());
            work->result = wView.error();
            return;
        }

        // Decode new picture.
        if (mPic == NULL) {
            // Init FFmpeg decoder.
            if (mDecInit == false)
            {
                if (mVideoInfo.width == 0 && mVideoInfo.height == 0 &&
                    mWidth && mHeight) {
                    CODEC2_LOG(CODEC2_LOG_INFO, "Set resolution to mVideoInfo [%d:%d]", mWidth, mHeight);
                    mVideoInfo.width = mWidth;
                    mVideoInfo.height = mHeight;
                }
                if (mFFmpegVideoDecoderInitFunc(mIntfImpl->ConvertComponentNameToMimeType(mDecoderName.c_str()), &mVideoInfo, &mCodec)) {
                    CODEC2_LOG(CODEC2_LOG_ERR, "FFmpeg decoder init failed for %s", mDecoderName.c_str());
                    mSignalledError = true;
                    work->workletsProcessed = 1u;
                    work->result = C2_CORRUPTED;
                    return;
                }
                mDecInit = true;
            }

            mPic = (VIDEO_FRAME_WRAPPER_T *)malloc(sizeof(VIDEO_FRAME_WRAPPER_T));
            if (mPic == NULL) {
                mSignalledError = true;
                work->workletsProcessed = 1u;
                work->result = C2_NO_MEMORY;
                return;
            }
            memset(mPic, 0, sizeof(VIDEO_FRAME_WRAPPER_T));
            mPic->pts = work->input.ordinal.timestamp.peeku();

            mTimeStart = systemTime();
            nsecs_t delay = mTimeStart - mTimeEnd;
            int size = mFFmpegVideoDecoderProcessFunc(inBuffer, inSize, mPic, mCodec);
            mTimeEnd = systemTime();
            nsecs_t decodeTime = mTimeEnd - mTimeStart;
            mTimeTotal += decodeTime;
            mTotalProcessedFrameNum++;
            CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1,
                "Average DecodeTime=%" PRId64 "us, DecodeTime=%" PRId64 "us, Delay=%" PRId64 "us, FrameIndex=%" PRId64", In_Size=%zu, Out_Size=%d, In_Pts=%" PRId64", Out_Pts=%" PRId64", Flags=%x",
                (int64_t)(mTimeTotal / mTotalProcessedFrameNum / 1000), (decodeTime / 1000), (delay / 1000), work->input.ordinal.frameIndex.peeku(), inSize, size,
                work->input.ordinal.timestamp.peeku(), mPic->pts, work->input.flags);
            if (size >= 0) {
                if (mPic->width != 0 && mPic->height != 0
                    && (mPic->width != mWidth ||  mPic->height != mHeight)) {
                    CODEC2_LOG(CODEC2_LOG_INFO, "Resolution changed from %d x %d to %d x %d", mWidth, mHeight, mPic->width, mPic->height);
                    mWidth = mPic->width;
                    mHeight = mPic->height;
                    work->workletsProcessed = 0u;

                    C2StreamPictureSizeInfo::output size(0u, mWidth, mHeight);
                    std::vector<std::unique_ptr<C2SettingResult>> failures;
                    c2_status_t err =
                        mIntfImpl->config({&size}, C2_MAY_BLOCK, &failures);
                    if (err == OK) {
                        work->worklets.front()->output.configUpdate.push_back(
                            C2Param::Copy(size));
                    } else {
                        CODEC2_LOG(CODEC2_LOG_ERR, "Cannot set width and height");
                        mSignalledError = true;
                        work->workletsProcessed = 1u;
                        work->result = C2_CORRUPTED;
                        free(mPic);
                        mPic = NULL;
                        return;
                    }
                    continue;
                }
                hasPicture = true;
                mFirstPictureReviced = true;
            } else {
                // Decode frame failed.
                CODEC2_LOG(CODEC2_LOG_ERR, "Decode failed, frame Index %" PRId64", In_Pts %" PRId64"",
                    work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
                free(mPic);
                mPic = NULL;
                if (flushPendingWork) {
                    break;
                }
            }
        } else {
            hasPicture = true;
            mFirstPictureReviced = true;
        }

        if (hasPicture) {
            uint8_t *dstY = const_cast<uint8_t *>(wView.data()[C2PlanarLayout::PLANE_Y]);
            uint8_t *dstU = const_cast<uint8_t *>(wView.data()[C2PlanarLayout::PLANE_U]);
            uint8_t *dstV = const_cast<uint8_t *>(wView.data()[C2PlanarLayout::PLANE_V]);

            size_t srcYStride = mPic->linesize[0];
            size_t srcUStride = mPic->linesize[1];
            size_t srcVStride = mPic->linesize[2];
            C2PlanarLayout layout = wView.layout();
            size_t dstYStride = layout.planes[C2PlanarLayout::PLANE_Y].rowInc;
            size_t dstUVStride = layout.planes[C2PlanarLayout::PLANE_U].rowInc;

            const uint8_t *srcY = (const uint8_t *)mPic->data[0];
            const uint8_t *srcU = (const uint8_t *)mPic->data[1];
            const uint8_t *srcV = (const uint8_t *)mPic->data[2];

            // Convert and fill yuv data.
            convertYUV420Planar8ToYV12(dstY, dstU, dstV, srcY, srcU, srcV, srcYStride, srcUStride,
                                       srcVStride, dstYStride, dstUVStride, mWidth, mHeight);
            // Set out pts
            work->input.ordinal.customOrdinal = mPic->pts;
            uint8_t *data = NULL;

            // For yuv dump
            if (mDumpYuvFp) {
             /* const uint8_t* const* data = wView.data();
                int size = mOutBlock->width() * mOutBlock->height() * 3 / 2;
                fwrite(data[0], size, 1, mDumpYuvFp); */
                int shift;
                for (int i = 0; i < 3; i++) {
                     shift = i>0 ? 1 : 0;
                     data = (uint8_t *)mPic->data[i];
                     for (int j = 0; j < mOutBlock->height()>>shift; j++) {
                          fwrite(data, sizeof(char), mOutBlock->width()>>shift, mDumpYuvFp);
                           data += mPic->linesize[i];
                     }
                }
            }

            // Free pic after yuv data filled.
            mFFmpegVideoDecoderFreeFrameFunc(mCodec);
            free(mPic);
            mPic = NULL;

            if (!mPendingWorkFrameIndexes.empty()) {
                if (!flushPendingWork) {
                    mPendingWorkFrameIndexes.push_back(work->input.ordinal.frameIndex.peeku());
                }
                finishWork(mPendingWorkFrameIndexes.front(), work);
                mPendingWorkFrameIndexes.pop_front();
            } else {
                finishWork(workIndex, work);
            }
        }
        // Exit directly if no need flushPendingWork or flush done.
        if (!flushPendingWork || mPendingWorkFrameIndexes.empty()) {
            break;
        }
    }

    if (eos) {
        drainInternal(DRAIN_COMPONENT_WITH_EOS, pool, work);
        mSignalledOutputEos = true;
    } else if (!hasPicture) {
        // Pending or drop frame when decode failed.
        // For VP8 first 3(ffmpeg_decode_thread_num - 1) frames decode failed case.
        if (!mFirstPictureReviced && mPendingWorkFrameIndexes.size() < MAX_WORK_PENDING_COUNT) {
            mPendingWorkFrameIndexes.push_back(work->input.ordinal.frameIndex.peeku());
        } else {
            mTotalDropedOutputFrameNum++;
            CODEC2_LOG(CODEC2_LOG_ERR, "Drop frame Index %" PRId64", In_Pts %" PRId64", total droped %" PRId64"",
                work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku(), mTotalDropedOutputFrameNum);
            fillEmptyWork(work);
        }
    }
}

c2_status_t C2SoftVdec::drainInternal(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool,
        const std::unique_ptr<C2Work> &work) {
    (void)pool;
    if (drainMode == NO_DRAIN) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Drain with DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }

    if (drainMode == DRAIN_COMPONENT_WITH_EOS &&
            work && work->workletsProcessed == 0u) {
        fillEmptyWork(work);
    }
    return C2_OK;
}


c2_status_t C2SoftVdec::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    return drainInternal(drainMode, pool, nullptr);
}

class C2SoftVdecFactory : public C2ComponentFactory {
public:
    C2SoftVdecFactory(C2String decoderName)
          : mDecoderName(decoderName),
            mReflector(std::static_pointer_cast<C2ReflectorHelper>(
                    GetCodec2VendorComponentStore()->getParamReflector())){};

    c2_status_t createComponent(c2_node_id_t id, std::shared_ptr<C2Component>* const component,
                                ComponentDeleter deleter) override {
        UNUSED(deleter);
        *component = C2SoftVdec::create(mDecoderName, id, std::make_shared<C2SoftVdec::IntfImpl>(mDecoderName, mReflector));
        return *component ? C2_OK : C2_NO_MEMORY;
    }
    c2_status_t createInterface(c2_node_id_t id,
                                std::shared_ptr<C2ComponentInterface>* const interface,
                                InterfaceDeleter deleter) override {
        UNUSED(deleter);
        *interface =
                std::shared_ptr<C2ComponentInterface>(new SimpleInterface<C2SoftVdec::IntfImpl>(
                        mDecoderName.c_str(), id,
                        std::make_shared<C2SoftVdec::IntfImpl>(mDecoderName, mReflector)));
        return C2_OK;
    }
    ~C2SoftVdecFactory() override = default;

private:
    const C2String mDecoderName;
    std::shared_ptr<C2ReflectorHelper> mReflector;
};

}  // namespace android


#define CreateC2SoftVdecFactory(type) \
        extern "C" ::C2ComponentFactory* CreateC2SoftVdec##type##Factory(bool secureMode) {\
             ALOGV("%s", __func__);\
             UNUSED(secureMode);\
             return new ::android::C2SoftVdecFactory(android::k##type##DecoderName);\
        }

#define DestroyC2SoftVdecFactory(type) \
    extern "C" void DestroyC2SoftVdec##type##Factory(::C2ComponentFactory* factory) {\
        ALOGV("%s", __func__);\
        delete factory;\
    }

CreateC2SoftVdecFactory(VP6A)
CreateC2SoftVdecFactory(VP6F)
CreateC2SoftVdecFactory(VP8)
CreateC2SoftVdecFactory(H263)
CreateC2SoftVdecFactory(RM10)
CreateC2SoftVdecFactory(RM20)
CreateC2SoftVdecFactory(RM30)
CreateC2SoftVdecFactory(RM40)
CreateC2SoftVdecFactory(WMV1)
CreateC2SoftVdecFactory(WMV2)
CreateC2SoftVdecFactory(WMV3)
CreateC2SoftVdecFactory(VC1)

DestroyC2SoftVdecFactory(VP6A)
DestroyC2SoftVdecFactory(VP6F)
DestroyC2SoftVdecFactory(VP8)
DestroyC2SoftVdecFactory(H263)
DestroyC2SoftVdecFactory(RM10)
DestroyC2SoftVdecFactory(RM20)
DestroyC2SoftVdecFactory(RM30)
DestroyC2SoftVdecFactory(RM40)
DestroyC2SoftVdecFactory(WMV1)
DestroyC2SoftVdecFactory(WMV2)
DestroyC2SoftVdecFactory(WMV3)
DestroyC2SoftVdecFactory(VC1)

