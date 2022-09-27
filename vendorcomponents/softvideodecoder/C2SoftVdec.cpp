/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
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

#include <c2logdebug.h>
#include <C2SoftVdec.h>
#include <C2SoftVdecInterfaceImpl.h>


#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

uint32_t android::C2SoftVdec::mDumpFileCnt = 0;

namespace android {

// static
std::atomic<int32_t> C2SoftVdec::sConcurrentInstances = 0;

// static
std::shared_ptr<C2Component> C2SoftVdec::create(
        const std::string& name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl) {
    static const int32_t kMaxConcurrentInstances =
            property_get_int32("vendor.codec2.soft.vdec.concurrent-instances", 2);
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
        mResolutionChanged(false),
        mDecInit(false),
        mPic(NULL),
        gAmFFmpegCodecLibHandler(NULL),
        mCodec(NULL),
        mExtraData(NULL),
        mDumpYuvFp(NULL) {
        sConcurrentInstances.fetch_add(1, std::memory_order_relaxed);
        memset(&mVideoInfo, 0, sizeof(VIDEO_INFO_T));

        CODEC2_LOG(CODEC2_LOG_INFO, "Create %s(%s)", __func__, name.c_str());

        propGetInt(CODEC2_LOGDEBUG_PROPERTY, &gloglevel);
        bool mDumpYuvEnable = property_get_bool("vendor.media.codec2.dumpyuv", false);
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
    onRelease();
    if (mExtraData) {
        free(mExtraData);
        mExtraData = NULL;
    }
    if (mDumpYuvFp) {
        fclose(mDumpYuvFp);
    }
    sConcurrentInstances.fetch_sub(1, std::memory_order_relaxed);
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
    return C2_OK;
}

bool C2SoftVdec::load_ffmpeg_decoder_lib(){
    if (gAmFFmpegCodecLibHandler) {
        return true;
    }

    gAmFFmpegCodecLibHandler = dlopen("libamffmpegcodec.so", RTLD_NOW);
    if (!gAmFFmpegCodecLibHandler) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Failed to open ffmpeg decoder lib, %s", dlerror());
        goto Error;
    }

    mFFmpegVideoDecoderInitFunc = NULL;
    mFFmpegVideoDecoderInitFunc = (ffmpeg_video_decoder_init_fn)dlsym(gAmFFmpegCodecLibHandler, "ffmpeg_video_decoder_init");
    if (mFFmpegVideoDecoderInitFunc == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Find lib err:,%s", dlerror());
        goto Error;
    }

    mFFmpegVideoDecoderProcessFunc = NULL;
    mFFmpegVideoDecoderProcessFunc = (ffmpeg_video_decoder_process_fn)dlsym(gAmFFmpegCodecLibHandler, "ffmpeg_video_decoder_process");
    if (mFFmpegVideoDecoderProcessFunc == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Find lib err,%s", dlerror());
        goto Error;
    }

    mFFmpegVideoDecoderFreeFrameFunc = NULL;
    mFFmpegVideoDecoderFreeFrameFunc = (ffmpeg_video_decoder_free_frame_fn)dlsym(gAmFFmpegCodecLibHandler, "ffmpeg_video_decoder_free_frame");
    if (mFFmpegVideoDecoderFreeFrameFunc == NULL) {
        CODEC2_LOG(CODEC2_LOG_ERR, "Find lib err,%s", dlerror());
        goto Error;
    }

    mFFmpegVideoDecoderCloseFunc = NULL;
    mFFmpegVideoDecoderCloseFunc = (ffmpeg_video_decoder_close_fn)dlsym(gAmFFmpegCodecLibHandler, "ffmpeg_video_decoder_close");
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
    mFFmpegVideoDecoderInitFunc = NULL;
    mFFmpegVideoDecoderProcessFunc = NULL;
    mFFmpegVideoDecoderFreeFrameFunc = NULL;
    mFFmpegVideoDecoderCloseFunc = NULL;
    if (gAmFFmpegCodecLibHandler != NULL) {
        dlclose(gAmFFmpegCodecLibHandler);
        gAmFFmpegCodecLibHandler = NULL;
    }
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
            if (buffer) {
                mOutIndex = index;
                C2WorkOrdinalStruct outOrdinal = work->input.ordinal;
                cloneAndSend(
                    mOutIndex, work,
                    FillWork(C2FrameData::FLAG_INCOMPLETE, outOrdinal, buffer));
                buffer.reset();
            }
        } else {
            fillWork(work);
        }
    } else {
        finish(index, fillWork);
    }
}

c2_status_t C2SoftVdec::ensureDecoderState(const std::shared_ptr<C2BlockPool> &pool) {
    if (!mFFmpegVideoDecoderInitFunc || !mFFmpegVideoDecoderProcessFunc || !mFFmpegVideoDecoderFreeFrameFunc) {
        ALOGE("not supposed to be here, invalid decoder context");
        return C2_CORRUPTED;
    }
    if (mOutBlock &&
            (mOutBlock->width() != ALIGN128(mWidth) || mOutBlock->height() != mHeight)) {
        mOutBlock.reset();
    }
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Start fetchGraphicBlock, Required (%dx%d)", ALIGN128(mWidth), mHeight);
    if (!mOutBlock) {
        uint32_t format = HAL_PIXEL_FORMAT_YV12;
        C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
        c2_status_t err =
            pool->fetchGraphicBlock(ALIGN128(mWidth), mHeight, format, usage, &mOutBlock);
        if (err != C2_OK) {
            CODEC2_LOG(CODEC2_LOG_ERR, "FetchGraphicBlock for Output failed with status %d", err);
            return err;
        }
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "FetchGraphicBlock done, Provided (%dx%d) Required (%dx%d)",
              mOutBlock->width(), mOutBlock->height(), ALIGN128(mWidth), mHeight);
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

    uint8_t *inBuffuer = const_cast<uint8_t *>(rView.data());
    bool codecConfig = ((work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) !=0);
    bool eos = ((work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
    bool hasPicture = false;

    // Config csd data
    if (codecConfig) {
        if (inSize > 0) {
            CODEC2_LOG(CODEC2_LOG_INFO, "Config num:%zu", inSize);
            if (mExtraData != NULL) {
                free(mExtraData);
            }
            mExtraData = (uint8_t *)malloc(inSize);
            memcpy(mExtraData, inBuffuer, inSize);
            mVideoInfo.extra_data = mExtraData;
            mVideoInfo.extra_data_size = inSize;
        }
        if ((mDecoderName.find("wmv3") != std::string::npos)
            || (mDecoderName.find("vc1") != std::string::npos)) {
            CODEC2_LOG(CODEC2_LOG_INFO, "For %s don't input config pkt to ffmpeg", mDecoderName.c_str());
            fillEmptyWork(work);
            return;
        }
    }

    // Loop for resolution changed case.
    while (inSize > 0) {
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
            // Init FFmpeg decoer.
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
            mPic->pts = (int64_t)work->input.ordinal.timestamp.peeku();

            mTimeStart = systemTime();
            nsecs_t delay = mTimeStart - mTimeEnd;
            int size = mFFmpegVideoDecoderProcessFunc(inBuffuer, inSize, mPic, mCodec);
            mTimeEnd = systemTime();
            nsecs_t decodeTime = mTimeEnd - mTimeStart;
            mTimeTotal += decodeTime;
            mTotalProcessedFrameNum++;
            CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1,
                "Average DecodeTime=%" PRId64 "us, DecodeTime=%" PRId64 "us, Delay=%" PRId64 "us, FrameIndex=%d, Size=%d, In_Pts=%" PRId64", Out_Pts=%" PRId64", Flags=%x",
                (int64_t)(mTimeTotal / mTotalProcessedFrameNum / 1000), (decodeTime / 1000), (delay / 1000), (int)work->input.ordinal.frameIndex.peeku(), size,
                (int64_t)work->input.ordinal.timestamp.peeku(), mPic->pts, work->input.flags);
            if (size > 0) {
                if (mPic->width != 0 && mPic->height != 0
                    && (mPic->width != mWidth ||  mPic->height != mHeight)) {
                    CODEC2_LOG(CODEC2_LOG_INFO, "Resolution changed from %d x %d to %d x %d", mWidth, mHeight, mPic->width, mPic->height);
                    mWidth = mPic->width;
                    mHeight = mPic->height;
                    mResolutionChanged = true;
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
            } else {
                // Decode frame failed.
                mTotalDropedOutputFrameNum++;
                CODEC2_LOG(CODEC2_LOG_ERR, "Decode failed, drop frame Index %d, In_Pts %" PRId64", total droped %" PRId64"",
                    (int)work->input.ordinal.frameIndex.peeku(), (int64_t)work->input.ordinal.timestamp.peeku(), mTotalDropedOutputFrameNum);
                free(mPic);
                mPic = NULL;;
            }
        } else {
            hasPicture = true;
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

            // For yuv dump
            if (mDumpYuvFp) {
                const uint8_t* const* data = wView.data();
                int size = mOutBlock->width() * mOutBlock->height() * 3 / 2;
                fwrite(data[0], 1, size, mDumpYuvFp);
            }

            // Free pic after yuv data filled.
            mFFmpegVideoDecoderFreeFrameFunc(mCodec);
            free(mPic);
            mPic = NULL;
            finishWork(workIndex, work);
        }

        // Reset decoder for resolution changed.
        if (mResolutionChanged) {
            resetDecoder();
            resetPlugin();
            mResolutionChanged = false;
        }
        // Work done, break.
        break;
    }

    if (eos) {
        drainInternal(DRAIN_COMPONENT_WITH_EOS, pool, work);
        mSignalledOutputEos = true;
    } else if (!hasPicture) {
        fillEmptyWork(work);
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

