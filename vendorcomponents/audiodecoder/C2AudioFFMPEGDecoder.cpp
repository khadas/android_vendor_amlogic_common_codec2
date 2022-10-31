/*
 * Copyright (C) 2022 Amlogic, Inc. All rights reserved.
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
#define LOG_TAG "Amlogic_C2AudioFFMPEGDecoder"
#include <log/log.h>

#include <inttypes.h>
#include <math.h>
#include <numeric>
#include <dlfcn.h>

#include <cutils/properties.h>
#include <media/stagefright/foundation/MediaDefs.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaErrors.h>
#include <utils/misc.h>

#include <C2PlatformSupport.h>
#include <C2AudioDecInterface.h>

#include "C2VendorSupport.h"
#include "C2AudioFFMPEGDecoder.h"
#include "C2VendorConfig.h"

#define MAX_CHANNEL_COUNT            8  /* maximum number of audio channels that can be decoded */

#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

#define LOGE ALOGE
#define LOGI ALOGI
#define LOGW ALOGW
#define LOGD ALOGD
#define LOGV ALOGV
#define LOG_LINE() ALOGD("[%s:%d]", __FUNCTION__, __LINE__);
#define BYTE_REV(a) ((((uint16_t)a) & 0xff) << 8 | ((uint16_t)a) >> 8)

#define AV_CODEC_ID_ADPCM_IMA_WAV 0x11001
#define AV_CODEC_ID_ADPCM_MS 0x11006
#define AV_CODEC_ID_MP2 0x15000
#define AV_CODEC_ID_WMA 0x15007
#define AV_CODEC_ID_WMAV2 0x15008
#define AV_CODEC_ID_COOK 0x15014
#define AV_CODEC_ID_WMAVOICE 0x15025
#define AV_CODEC_ID_WMAPRO 0x15026
#define AV_CODEC_ID_WMALOSSLESS 0x15027
#define AV_CODEC_ID_APE 0x15021
#define MAX_AUDIO_BUFFER_SIZE 1024 * 64
#define WMA_SKIP_TIME_THRESHOLD 3

const char *MEDIA_MIMETYPE_AUDIO_FFMPEG = "audio/ffmpeg";
constexpr char COMPONENT_NAME[] = "c2.amlogic.audio.decoder.ffmpeg";
static int prevSampleRate = 0;
static int prevNumChannels = 0;

namespace android {


class C2AudioFFMPEGDecoder::IntfImpl : public AudioDecInterface<void>::BaseParams {
public:
    static C2R ExtraDataInputSetter(bool mayBlock, C2P<C2ExtraData::input> &me) {
        (void)mayBlock;
        (void)me;
        return C2R::Ok();
    }

    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper)
        : AudioDecInterface<void>::BaseParams(
                helper,
                COMPONENT_NAME,
                C2Component::KIND_DECODER,
                C2Component::DOMAIN_AUDIO,
                MEDIA_MIMETYPE_AUDIO_FFMPEG) {

        addParameter(
                DefineParam(mSampleRate, C2_PARAMKEY_SAMPLE_RATE)
                .withDefault(new C2StreamSampleRateInfo::output(0u, 44100))
                .withFields({C2F(mSampleRate, value).oneOf({
                    7350, 8000, 11025, 12000, 16000, 22050, 24000, 32000,
                    44100, 48000, 64000, 88200, 96000
                })})
                .withSetter(Setter<decltype(*mSampleRate)>::NonStrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mChannelCount, C2_PARAMKEY_CHANNEL_COUNT)
                .withDefault(new C2StreamChannelCountInfo::output(0u, 1))
                .withFields({C2F(mChannelCount, value).inRange(1, MAX_CHANNEL_COUNT)})
                .withSetter(Setter<decltype(*mChannelCount)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mMaxChannelCount, C2_PARAMKEY_MAX_CHANNEL_COUNT)
                .withDefault(new C2StreamMaxChannelCountInfo::input(0u, MAX_CHANNEL_COUNT))
                .withFields({C2F(mMaxChannelCount, value).inRange(1, MAX_CHANNEL_COUNT)})
                .withSetter(Setter<decltype(*mMaxChannelCount)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
                .withDefault(new C2StreamBitrateInfo::input(0u, 64000))
                .withFields({C2F(mBitrate, value).inRange(8000, 960000)})
                .withSetter(Setter<decltype(*mBitrate)>::NonStrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mInputMaxBufSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
                .withConstValue(new C2StreamMaxBufferSizeInfo::input(0u, 2*1024*1024))
                .build());

        addParameter(DefineParam(mChannelMask, C2_PARAMKEY_CHANNEL_MASK)
                .withDefault(new C2StreamChannelMaskInfo::output(0u, 0))
                .withFields({C2F(mChannelMask, value).inRange(0, 4294967292)})
                .withSetter(Setter<decltype(*mChannelMask)>::StrictValueWithNoDeps)
                .build());

        addParameter(DefineParam(mSdkCodecId, C2_PARAMKEY_VENDOR_CODECID)
                .withDefault(new C2SdkCodecId::input(0))
                .withFields({C2F(mSdkCodecId, value).any()})
                .withSetter(Setter<decltype(*mSdkCodecId)>::StrictValueWithNoDeps)
                .build());

        addParameter(DefineParam(mBlockAlign, C2_PARAMKEY_VENDOR_BLOCK_ALIGN)
                .withDefault(new C2BlockAlign::input(0))
                .withFields({C2F(mBlockAlign, value).any()})
                .withSetter(Setter<decltype(*mBlockAlign)>::StrictValueWithNoDeps)
                .build());

        addParameter(DefineParam(mExtraDataSize, C2_PARAMKEY_VENDOR_EXTRA_DATA_SIZE)
                .withDefault(new C2ExtraDataSize::input(0))
                .withFields({C2F(mExtraDataSize, value).any()})
                .withSetter(Setter<decltype(*mExtraDataSize)>::StrictValueWithNoDeps)
                .build());

        mExtraData = C2ExtraData::input::AllocShared(0);
        addParameter(
                DefineParam(mExtraData, C2_PARAMKEY_VENDOR_EXTRA_DATA)
                .withDefault(mExtraData)
                .withFields({
                    C2F(mExtraData, m.value).any(),
                })
                .withSetter(ExtraDataInputSetter)
                .build());
    }


    u_int32_t getMaxChannelCount() const { return mMaxChannelCount->value; }
    int32_t getSdkCodecId() const { return mSdkCodecId->value; }
    int32_t getExtraDataSize() const { return mExtraDataSize->value; }
    void getExtraData(uint8_t** pbuf, uint32_t* plen) const {
        if (pbuf == NULL || plen == NULL) {
            return;
        }
        *pbuf = mExtraData->m.value;
        *plen = mExtraData->flexCount();

        return ;
    }
    int32_t getChannelCount() const { return mChannelCount->value; }
    int32_t getSampleRate() const { return mSampleRate->value; }
    int32_t getBitRate() const { return mBitrate->value; }
    int32_t getBlockAlign() const { return mBlockAlign->value; }

private:
    std::shared_ptr<C2StreamSampleRateInfo::output> mSampleRate;
    std::shared_ptr<C2StreamChannelCountInfo::output> mChannelCount;
    std::shared_ptr<C2StreamBitrateInfo::input> mBitrate;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mInputMaxBufSize;
    std::shared_ptr<C2StreamMaxChannelCountInfo::input> mMaxChannelCount;
    std::shared_ptr<C2StreamChannelMaskInfo::output> mChannelMask;
    std::shared_ptr<C2SdkCodecId::input> mSdkCodecId;
    std::shared_ptr<C2ExtraDataSize::input> mExtraDataSize;
    std::shared_ptr<C2ExtraData::input> mExtraData;
    std::shared_ptr<C2BlockAlign::input> mBlockAlign;
};


static int amsysfs_set_sysfs_str(const char *path, const char *val) {
    int fd, bytes;
    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0) {
        bytes = write(fd, val, strlen(val));
        close(fd);
        return 0;
    } else {
        ALOGE("unable to open file %s,err: %s", path, strerror(errno));
    }
    return -1;
}

static void dump(const char * path, char *data, int size)
{
    FILE *fp = NULL;
    fp = fopen(path, "a+");
    if (fp != NULL) {
        size_t  write_size = fwrite(data, sizeof(char), (size_t)size, fp);
        if (write_size != (size_t)size)
            ALOGE("error: write data to file failed[want:%d]-[ret:%zu]-[strerror(errno):%s]\n", size, write_size, strerror(errno));
        fclose(fp);
    }else {
        ALOGE("error: open file failed\n");
    }
}


C2AudioFFMPEGDecoder::C2AudioFFMPEGDecoder(
        const char *name,
        c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl)
    : C2AudioDecComponent(std::make_shared<AudioDecInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl),
    mComponentName(name),
    mMimeType("audio/ffmpeg"),
    debug_print(0),
    debug_dump(0),
    mSeeking(false),
    mDecoderState(DECODER_StateInvalid),
    mSetUp(false),
    mIsSecure(false),
    mClearBuffer(NULL),
    mClearLen(0),
    mNumFramesOutput(0),
    mCodec(NULL),
    mDecodeFilledTotalTimeUs(0),
    mDecoderFilledTimeUs(0)
{
    ALOGV("%s() %d  name:%s", __func__, __LINE__, mComponentName);
    {
        AutoMutex l(mSetUpLock);
        initializeState_l();
    }

    mOutBufferLen = 1024*1024*2;//2M size
    mOutBuffer = (char *)malloc(mOutBufferLen);
    memset(mOutBuffer, 0, sizeof(mOutBuffer));
}

C2AudioFFMPEGDecoder::~C2AudioFFMPEGDecoder() {
    ALOGV("%s() %d", __func__, __LINE__);
    if (mOutBuffer != NULL) {
        free(mOutBuffer);
        mOutBuffer = NULL;
    }

    onRelease();
    ALOGV("%s() %d  exit", __func__, __LINE__);
}

void C2AudioFFMPEGDecoder::initializeState_l() {
    LOG_LINE();
    AutoMutex l(mConfigLock);
    if (load_ffmpeg_decoder_lib()) {
        mAInfo = new AUDIO_INFO_T;
        if (mAInfo == NULL) {
            delete mAInfo;
        }
    } else {
        ALOGE("%s load_ffmpeg_decoder_lib failed, errno:%s", __func__, strerror(errno));
    }
}

bool C2AudioFFMPEGDecoder::tearDownAudioDecoder_l() {
    LOG_LINE();
    if ( mAInfo !=NULL ) {
        delete mAInfo;
        mAInfo = NULL;
    }
    if (gAmFFmpegCodecLibHandler != NULL) {
        unload_ffmpeg_decoder_lib();
    }
    if (mClearBuffer != NULL) {
        free(mClearBuffer);
        mClearBuffer = NULL;
        mClearLen = 0;
    }
    return true;
}

bool C2AudioFFMPEGDecoder::unload_ffmpeg_decoder_lib(){
    LOG_LINE();
    if (ffmpeg_decoder_close != NULL)
        (*ffmpeg_decoder_close)(mCodec);
    mCodec = NULL;
    ffmpeg_decoder_init = NULL;
    ffmpeg_decoder_process = NULL;
    ffmpeg_decoder_close = NULL;
    if (gAmFFmpegCodecLibHandler != NULL) {
        dlclose(gAmFFmpegCodecLibHandler);
        gAmFFmpegCodecLibHandler = NULL;
    }
    return true;
}

bool C2AudioFFMPEGDecoder::load_ffmpeg_decoder_lib(){
    gAmFFmpegCodecLibHandler = NULL;
    memset(&pcm_out_info, 0, sizeof(pcm_out_info));
    gAmFFmpegCodecLibHandler = dlopen("libamffmpegcodec.so", RTLD_NOW);
    if (!gAmFFmpegCodecLibHandler) {
        LOGE("failed to open ffmpeg decoder lib, %s\n", dlerror());
        goto Error;
    }
    ffmpeg_decoder_init = (int (*)(const char *, AUDIO_INFO *,AmAudioCodec **))dlsym(gAmFFmpegCodecLibHandler, "ffmpeg_decoder_init");
    if (ffmpeg_decoder_init == NULL) {
        LOGE("find lib err:,%s\n", dlerror());
        goto Error;
    }
    ffmpeg_decoder_process = (int (*)(char * ,int ,int *,char *,int *,struct pcm_info *,AmAudioCodec *))dlsym(gAmFFmpegCodecLibHandler, "ffmpeg_decoder_process");
    if (ffmpeg_decoder_process == NULL) {
        LOGE("find lib err,%s\n", dlerror());
        goto Error;
    }
    ffmpeg_decoder_close = (int (*)(AmAudioCodec *))dlsym(gAmFFmpegCodecLibHandler, "ffmpeg_decoder_close");
    if (ffmpeg_decoder_close == NULL) {
        LOGE("find lib err:%s\n", dlerror());
        goto Error;
    }
    return true;

Error:
    unload_ffmpeg_decoder_lib();
    return false;
}

bool C2AudioFFMPEGDecoder::setUpAudioDecoder_l() {
    char value[PROPERTY_VALUE_MAX];
    uint8_t *data = nullptr;
    uint32_t dataLen = 0;
    int ret  = 0;

    debug_print = 0;
    debug_dump = 0;
    memset(value,0,sizeof(value));
#ifdef SUPPORT_STANDARD_PROP
    if ((property_get("vendor.media.codec2.audio.debug",value,NULL) > 0) &&
#else
    if ((property_get("media.codec2.audio.debug",value,NULL) > 0) &&
#endif
        (!strcmp(value,"1")||!strcmp(value,"true"))) {
        debug_print = 1;
    }
    memset(value,0,sizeof(value));
#ifdef SUPPORT_STANDARD_PROP
    if ((property_get("vendor.media.codec2.audio.dump",value,NULL) > 0) &&
#else
    if ((property_get("media.codec2.audio.dump",value,NULL) > 0) &&
#endif
        (!strcmp(value,"1")||!strcmp(value,"true"))) {
        debug_dump = 1;
    }

    if (NULL == mAInfo ||  NULL == mIntf) {
        ALOGE("%s  mAInfo or mIntf is null, so exit directly", __func__);
        goto Error;
    }
    mAInfo->extradata_size = mIntf->getExtraDataSize();
    mIntf->getExtraData(&data, &dataLen);
    if (dataLen > 0) {
        mAInfo->extradata_size = dataLen;
        memcpy(mAInfo->extradata, data, dataLen);
    }

    mAInfo->codec_id = mIntf->getSdkCodecId();
    mAInfo->blockalign = mIntf->getBlockAlign();
    mAInfo->channels = mIntf->getChannelCount();
    mAInfo->samplerate = mIntf->getSampleRate();
    mAInfo->bitrate = mIntf->getBitRate();
    mAInfo->bitspersample =  mIntf->getBitRate() / (mIntf->getSampleRate() * mIntf->getChannelCount());
    if ( mAInfo->codec_id == AV_CODEC_ID_ADPCM_IMA_WAV || mAInfo->codec_id == AV_CODEC_ID_ADPCM_MS)
        mAInfo->bitspersample = 4;

    ALOGI("mAInfo codec_id:(0x%x %d) blockalign:%d bitspersample:%d channelCount:%d SampleRate:%d BitRate:%d",
        mAInfo->codec_id, mAInfo->codec_id, mAInfo->blockalign, mAInfo->bitspersample, mIntf->getChannelCount(), mIntf->getSampleRate(), mIntf->getBitRate());

    ret = (*ffmpeg_decoder_init)(mMimeType, mAInfo, &mCodec);
    ALOGI("ffmpeg audio_decode_init return %d", ret);
    return true;
Error:
    return false;
}

bool C2AudioFFMPEGDecoder::setUp() {
    LOG_LINE();
    AutoMutex l(mSetUpLock);
    if (mSetUp) {
        LOGW("Trying to set up stream when you already have.");
        return false;
    }

    if (!setUpAudioDecoder_l()) {
        LOGE("setUpFFMPEGAudioDecoder_l failed.");
        tearDownAudioDecoder_l();
        return false;
    }
    ALOGI("C2AudioFFMPEGDecoder setUp done\n");
    mSetUp = true;
    return true;
}

bool C2AudioFFMPEGDecoder::tearDown() {
    LOG_LINE();
    AutoMutex l(mSetUpLock);
    if (mSetUp) {
        tearDownAudioDecoder_l();
    }
    return true;
}

bool C2AudioFFMPEGDecoder::isSetUp() {
    AutoMutex l(mSetUpLock);
    return mSetUp;
}

c2_status_t C2AudioFFMPEGDecoder::onInit() {
    ALOGV("%s() %d", __func__, __LINE__);

    status_t err = initDecoder();
    prevSampleRate = 0;
    prevNumChannels = 0;

    ALOGV("%s() %d exit", __func__, __LINE__);
    return err == OK ? C2_OK : C2_CORRUPTED;
}

c2_status_t C2AudioFFMPEGDecoder::onStop() {
    ALOGV("%s() %d", __func__, __LINE__);

    mBuffersInfo.clear();
    mAbortPlaying = true;

    return C2_OK;
}

void C2AudioFFMPEGDecoder::onReset() {
    ALOGV("%s() %d", __func__, __LINE__);
    (void)onStop();

    if (ffmpeg_decoder_close != NULL)
        (*ffmpeg_decoder_close)(mCodec);

    if ((mAInfo->codec_id == AV_CODEC_ID_MP2
        || mAInfo->codec_id == AV_CODEC_ID_WMA
        || mAInfo->codec_id == AV_CODEC_ID_WMAPRO
        || mAInfo->codec_id == AV_CODEC_ID_ADPCM_IMA_WAV
        || mAInfo->codec_id == AV_CODEC_ID_ADPCM_MS
        || mAInfo->codec_id == AV_CODEC_ID_COOK) && mComponentName) {
        int ret = (*ffmpeg_decoder_init)(mMimeType, mAInfo, &mCodec);
        ALOGI("ffmpeg audio_decode_init return %d", ret);
    }
}

void C2AudioFFMPEGDecoder::onRelease() {
    LOG_LINE();
    onStop();
    tearDown();//move to here from releaseResources, for SWPL-40408
    LOG_LINE();

    mDecodingErrors = 0;
    mDecodedFrames= 0;
    char value[PROPERTY_VALUE_MAX];
    if (property_get(AML_DEBUG_AUDIOINFO_REPORT_PROPERTY, value, NULL)) {
        sprintf(sysfs_buf, "decoded_err %d", mDecodingErrors);
        amsysfs_set_sysfs_str(REPORT_DECODED_INFO, sysfs_buf);
        sprintf(sysfs_buf, "decoded_frames %d", mDecodedFrames);
        amsysfs_set_sysfs_str(REPORT_DECODED_INFO, sysfs_buf);
    }
    memset(sysfs_buf, 0, sizeof(sysfs_buf));

    mSetUp = false;
}

status_t C2AudioFFMPEGDecoder::initDecoder() {
    LOG_LINE();
    status_t status = UNKNOWN_ERROR;

    if (setUp()) {
    }

    if (!isSetUp()) {
        LOG_LINE();
        return C2_OMITTED;
    }

    status = OK;
    return status;
}

void C2AudioFFMPEGDecoder::drainOutBuffer(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool,
        bool eos) {

    while (!mBuffersInfo.empty()) {
        Info &outInfo = mBuffersInfo.front();
        int numFrames = outInfo.decodedSizes.size();
        int outputDataSize = mOutSize;
        if (debug_print) {
            ALOGV("%s outputDataSize:%d,  outInfo numFrames:%d,frameIndex = %" PRIu64 "",__func__, outputDataSize, numFrames, outInfo.frameIndex);
        }

        std::shared_ptr<C2LinearBlock> block;
        std::function<void(const std::unique_ptr<C2Work>&)> fillWork =
            [&block, outputDataSize, pool, this]()
                    -> std::function<void(const std::unique_ptr<C2Work>&)> {
                auto fillEmptyWork = [](
                        const std::unique_ptr<C2Work> &work, c2_status_t err) {
                    work->result = err;
                    C2FrameData &output = work->worklets.front()->output;
                    output.flags = work->input.flags;
                    output.buffers.clear();
                    output.ordinal = work->input.ordinal;

                    work->workletsProcessed = 1u;
                };

                using namespace std::placeholders;
                if (outputDataSize == 0) {
                    return std::bind(fillEmptyWork, _1, C2_OK);
                }

                // TODO: error handling, proper usage, etc.
                C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
                size_t bufferSize = mOutSize;
                c2_status_t err = pool->fetchLinearBlock(bufferSize, usage, &block);
                if (err != C2_OK) {
                    ALOGE("failed to fetch a linear block (%d)", err);
                    return std::bind(fillEmptyWork, _1, C2_NO_MEMORY);
                }
                C2WriteView wView = block->map().get();
                int16_t *outBuffer = reinterpret_cast<int16_t *>(wView.data());
                memcpy(outBuffer, mOutBuffer, mOutSize);
                if (debug_dump == 1) {
                    dump("/data/vendor/audiohal/c2_audio_out_2.pcm", (char *)outBuffer, mOutSize);
                }

                return [buffer = createLinearBuffer(block, 0, bufferSize)](
                        const std::unique_ptr<C2Work> &work) {
                    work->result = C2_OK;
                    C2FrameData &output = work->worklets.front()->output;
                    output.flags = work->input.flags;
                    output.buffers.clear();
                    output.buffers.push_back(buffer);
                    output.ordinal = work->input.ordinal;
                    work->workletsProcessed = 1u;
                };
            }();

        if (work && work->input.ordinal.frameIndex == c2_cntr64_t(outInfo.frameIndex)) {
            fillWork(work);
        } else {
            finish(outInfo.frameIndex, fillWork);
        }

        mBuffersInfo.pop_front();
        if (debug_print) {
            ALOGV("%s  mBuffersInfo is %s, out timestamp %" PRIu64 " / %u", __func__, mBuffersInfo.empty()?"null":"not null", outInfo.timestamp, block ? block->capacity() : 0);
        }
    }
}

void C2AudioFFMPEGDecoder::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    int ret = 0;
    // Initialize output work
    work->result = C2_OK;
    work->workletsProcessed = 1u;
    work->worklets.front()->output.configUpdate.clear();
    work->worklets.front()->output.flags = work->input.flags;
    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;

    uint8* inBuffer = NULL;
    C2ReadView view = mDummyReadView;
    size_t offset = 0u;
    size_t inBuffer_nFilledLen = 0u;
    //uint32_t out_nFilledLen = 0u;
    if (!work->input.buffers.empty()) {
        view = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inBuffer_nFilledLen = view.capacity();
    }
    inBuffer = const_cast<uint8 *>(view.data() + offset);


    if (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) {
        ALOGI("ffmpeg_decoder handle csd data!");
        if (NULL != mAInfo) {
            mAInfo->extradata_size = (int32_t)inBuffer_nFilledLen;
            if (inBuffer_nFilledLen > 0) {
                mAInfo->extradata_size = (int32_t)inBuffer_nFilledLen;
                memcpy(mAInfo->extradata, inBuffer, inBuffer_nFilledLen);
            }
            ALOGI("mAInfo codec_id:(0x%x %d) blockalign:%d bitspersample:%d channelCount:%d SampleRate:%d BitRate:%d",
                mAInfo->codec_id, mAInfo->codec_id, mAInfo->blockalign, mAInfo->bitspersample, mIntf->getChannelCount(), mIntf->getSampleRate(), mIntf->getBitRate());

            if (ffmpeg_decoder_close != NULL && mCodec != NULL) {
                (*ffmpeg_decoder_close)(mCodec);
                mCodec = NULL;
            }

            int ret = (*ffmpeg_decoder_init)(mMimeType, mAInfo, &mCodec);
            ALOGI("ffmpeg audio_decode_init return %d", ret);
        }
        return;
    }


    Info inInfo;
    inInfo.frameIndex = work->input.ordinal.frameIndex.peeku();
    inInfo.timestamp = work->input.ordinal.timestamp.peeku();
    inInfo.bufferSize = inBuffer_nFilledLen;
    inInfo.decodedSizes.clear();
    if (debug_print) {
        ALOGV("%s() inInfo.bufferSize:%zu, frameIndex:%" PRIu64 ", timestamp:%" PRIu64 "", __func__, inInfo.bufferSize, inInfo.frameIndex, inInfo.timestamp);
    }


    //uint32_t inBuffer_offset = 0;
    //uint32_t out_offset = 0;
    //uint64_t out_timestamp = 0;
    char *decoder_buffer;
    decoder_buffer = (char *)inBuffer;
    mOutSize = 0;

    do {
        int usedsize = 0;
        int outsize = 0;
        ret = (*ffmpeg_decoder_process)( decoder_buffer,
                                         inBuffer_nFilledLen,
                                         &usedsize,
                                         (char *)(mOutBuffer+mOutSize),
                                         &outsize,
                                         (struct pcm_info *)&pcm_out_info,
                                          mCodec);


        if (ret < 0 || usedsize < 0) {//can't decode this frame
            inBuffer_nFilledLen -= inBuffer_nFilledLen;
        } else {
            inBuffer_nFilledLen -= usedsize;
        }
        mOutSize += outsize;

        if (debug_dump == 1) {
            dump("/data/vendor/audiohal/c2_audio_decoded.pcm", (char *)(mOutBuffer+mOutSize), outsize);
        }
        if (debug_print) {
            ALOGV("%s decoder_buffer:%p  ret:%d  usedsize:%d, inBuffer_nFilledLen:%zu  ----   mOutSize:%d  outsize:%d, pcm_out_info.sample_rate:%d channel_num:%d", __func__,
                decoder_buffer, ret, usedsize, inBuffer_nFilledLen, mOutSize, outsize, pcm_out_info.sample_rate,pcm_out_info.channel_num);
        }

        //update out config.
        if (!pcm_out_info.sample_rate || !pcm_out_info.channel_num) {
            ALOGW("%s Invalid data frame", __func__);
        } else if ((pcm_out_info.sample_rate != prevSampleRate) ||
                   (pcm_out_info.channel_num != prevNumChannels)) {
            ALOGI("Reconfiguring decoder: %d->%d Hz, %d->%d channels",
                  prevSampleRate, pcm_out_info.sample_rate,
                  prevNumChannels, pcm_out_info.channel_num);

            prevSampleRate = pcm_out_info.sample_rate;
            prevNumChannels = pcm_out_info.channel_num;

            C2StreamSampleRateInfo::output sampleRateInfo(0u, pcm_out_info.sample_rate);
            C2StreamChannelCountInfo::output channelCountInfo(0u, pcm_out_info.channel_num);
            C2StreamChannelMaskInfo::output channelMaskInfo(0u,
                    maskFromCount(pcm_out_info.channel_num));
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            c2_status_t err = mIntf->config(
                    { &sampleRateInfo, &channelCountInfo, &channelMaskInfo },
                    C2_MAY_BLOCK,
                    &failures);
            if (err == OK) {
                C2FrameData &output = work->worklets.front()->output;
                output.configUpdate.push_back(C2Param::Copy(sampleRateInfo));
                output.configUpdate.push_back(C2Param::Copy(channelCountInfo));
                output.configUpdate.push_back(C2Param::Copy(channelMaskInfo));
            } else {
                ALOGE("Config Update failed");
                work->result = C2_CORRUPTED;
                return;
            }
        }
    } while (inBuffer_nFilledLen > 0);
    mBuffersInfo.push_back(std::move(inInfo));

    if (eos) {
        drainEos(DRAIN_COMPONENT_WITH_EOS, pool, work);
    } else {
        drainOutBuffer(work, pool, false);
    }
}

c2_status_t C2AudioFFMPEGDecoder::drainEos(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool,
        const std::unique_ptr<C2Work> &work) {
    LOG_LINE();
    bool eos = (drainMode == DRAIN_COMPONENT_WITH_EOS);

    drainOutBuffer(work, pool, eos);
    if (eos) {
        auto fillEmptyWork = [](const std::unique_ptr<C2Work> &work) {
            work->worklets.front()->output.flags = work->input.flags;
            work->worklets.front()->output.buffers.clear();
            work->worklets.front()->output.ordinal = work->input.ordinal;
            work->workletsProcessed = 1u;
        };
        while (mBuffersInfo.size() > 1u) {
            finish(mBuffersInfo.front().frameIndex, fillEmptyWork);
            mBuffersInfo.pop_front();
        }
        if (work && work->workletsProcessed == 0u) {
            fillEmptyWork(work);
        }
        mBuffersInfo.clear();
    }

    return C2_OK;
}

c2_status_t C2AudioFFMPEGDecoder::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    LOG_LINE();

    return C2_OK;
}

c2_status_t C2AudioFFMPEGDecoder::onFlush_sm() {
    LOG_LINE();
    mBuffersInfo.clear();

    return C2_OK;
}

void C2AudioFFMPEGDecoder::drainDecoder() {
    LOG_LINE();
}

// definitions based on android.media.AudioFormat.CHANNEL_OUT_*
#define CHANNEL_OUT_FL  0x4
#define CHANNEL_OUT_FR  0x8
#define CHANNEL_OUT_FC  0x10
#define CHANNEL_OUT_LFE 0x20
#define CHANNEL_OUT_BL  0x40
#define CHANNEL_OUT_BR  0x80
#define CHANNEL_OUT_SL  0x800
#define CHANNEL_OUT_SR  0x1000

uint32_t C2AudioFFMPEGDecoder::maskFromCount(uint32_t channelCount) {
    // KEY_CHANNEL_MASK expects masks formatted according to Java android.media.AudioFormat
    // where the two left-most bits are 0 for output channel mask
    switch (channelCount) {
        case 1: // mono is front left
            return (CHANNEL_OUT_FL);
        case 2: // stereo
            return (CHANNEL_OUT_FL | CHANNEL_OUT_FR);
        case 4: // 4.0 = stereo with backs
            return (CHANNEL_OUT_FL | CHANNEL_OUT_FC
                    | CHANNEL_OUT_BL | CHANNEL_OUT_BR);
        case 5: // 5.0
            return (CHANNEL_OUT_FL | CHANNEL_OUT_FC | CHANNEL_OUT_FR
                    | CHANNEL_OUT_BL | CHANNEL_OUT_BR);
        case 6: // 5.1 = 5.0 + LFE
            return (CHANNEL_OUT_FL | CHANNEL_OUT_FC | CHANNEL_OUT_FR
                    | CHANNEL_OUT_BL | CHANNEL_OUT_BR
                    | CHANNEL_OUT_LFE);
        case 7: // 7.0 = 5.0 + Sides
            return (CHANNEL_OUT_FL | CHANNEL_OUT_FC | CHANNEL_OUT_FR
                    | CHANNEL_OUT_BL | CHANNEL_OUT_BR
                    | CHANNEL_OUT_SL | CHANNEL_OUT_SR);
        case 8: // 7.1 = 7.0 + LFE
            return (CHANNEL_OUT_FL | CHANNEL_OUT_FC | CHANNEL_OUT_FR
                    | CHANNEL_OUT_BL | CHANNEL_OUT_BR | CHANNEL_OUT_SL | CHANNEL_OUT_SR
                    | CHANNEL_OUT_LFE);
        default:
            return 0;
    }
}


class C2AudioFFMPEGDecFactory : public C2ComponentFactory {
public:
    C2AudioFFMPEGDecFactory() : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
            GetCodec2VendorComponentStore()->getParamReflector())) {
            ALOGI("in %s ", __func__);
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
            UNUSED(deleter);
            ALOGI("in %s ", __func__);
        *component = std::shared_ptr<C2Component>(
                new C2AudioFFMPEGDecoder(COMPONENT_NAME,
                              id,
                              std::make_shared<C2AudioFFMPEGDecoder::IntfImpl>(mHelper)));
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id, std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
            UNUSED(deleter);
            ALOGI("in %s ", __func__);
        *interface = std::shared_ptr<C2ComponentInterface>(
                new AudioDecInterface<C2AudioFFMPEGDecoder::IntfImpl>(
                        COMPONENT_NAME, id, std::make_shared<C2AudioFFMPEGDecoder::IntfImpl>(mHelper)));
        return C2_OK;
    }

    virtual ~C2AudioFFMPEGDecFactory() override = default;

private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
};


}  // namespace android

__attribute__((cfi_canonical_jump_table))
extern "C" ::C2ComponentFactory* CreateC2AudioDecoderFFMPEGFactory() {
    ALOGV("in %s", __func__);
    return new ::android::C2AudioFFMPEGDecFactory();
}

__attribute__((cfi_canonical_jump_table))
extern "C" void DestroyC2AudioDecoderFFMPEGFactory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}


