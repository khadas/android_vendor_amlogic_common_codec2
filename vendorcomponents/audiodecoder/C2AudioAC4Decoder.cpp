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
#define LOG_TAG "C2AudioAC4Decoder"
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
#include "C2AudioAC4Decoder.h"
#include "aml_ac4_decoder_api.h"
#include "C2VendorConfig.h"
#include "AmlAudioCommon.h"

#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

#define AC4_MAX_CHANNEL_COUNT   (8)
#define AC4_MAX_FRAME_LENGTH    (16384)
#define AC4_MAX_OUTPUT_SAMPLES  (2230)

#define AC4_OUT_BUFFER_SIZE     (AC4_MAX_OUTPUT_SAMPLES * AC4_MAX_CHANNEL_COUNT * 2)
#define AC4_IN_BUFFER_SIZE      (AC4_MAX_FRAME_LENGTH * 2)

#define DOLBY_MS12_V24_LIB_PATH   "/dev/audio_utils"
#define DOLBY_MS12_V24_LIB_PATH_A "/odm/lib/ms12/libdolbyms12.so" //MS12 v2.4
#define DOLBY_MS12_V24_LIB_PATH_B "/vendor/lib/ms12/libdolbyms12.so" //MS12 v2.4

constexpr char COMPONENT_NAME_AC4[] = "c2.amlogic.audio.decoder.ac4";

namespace android {

static void dump(const char *path, char *data, int size) {
    FILE *fp = NULL;
    fp = fopen(path, "a+");
    if (fp != NULL) {
        size_t  write_size = fwrite(data, sizeof(char), (size_t)size, fp);
        if (write_size != (size_t)size)
            C2AUDIO_LOGE("error: write data to file failed[want:%d]-[ret:%zu]-[strerror(errno):%s]\n", size, write_size, strerror(errno));
        fclose(fp);
    }else {
        C2AUDIO_LOGE("error: open file failed\n");
    }
}

class C2AudioAC4Decoder::IntfImpl : public AudioDecInterface<void>::BaseParams {
public:
    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper)
        : AudioDecInterface<void>::BaseParams(
                helper,
                COMPONENT_NAME_AC4,
                C2Component::KIND_DECODER,
                C2Component::DOMAIN_AUDIO,
                MEDIA_MIMETYPE_AUDIO_AC4) {

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
                .withFields({C2F(mChannelCount, value).inRange(1, AC4_MAX_CHANNEL_COUNT)})
                .withSetter(Setter<decltype(*mChannelCount)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mMaxChannelCount, C2_PARAMKEY_MAX_CHANNEL_COUNT)
                .withDefault(new C2StreamMaxChannelCountInfo::input(0u, AC4_MAX_CHANNEL_COUNT))
                .withFields({C2F(mMaxChannelCount, value).inRange(1, AC4_MAX_CHANNEL_COUNT)})
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
                .withConstValue(new C2StreamMaxBufferSizeInfo::input(0u, AC4_MAX_FRAME_LENGTH))
                .build());

        addParameter(DefineParam(mChannelMask, C2_PARAMKEY_CHANNEL_MASK)
                .withDefault(new C2StreamChannelMaskInfo::output(0u, 0))
                .withFields({C2F(mChannelMask, value).inRange(0, 4294967292)})
                .withSetter(Setter<decltype(*mChannelMask)>::StrictValueWithNoDeps)
                .build());

        addParameter(DefineParam(mPassthroughEnable, C2_PARAMKEY_VENDOR_PASSTHROUGH_ENABLE)
                .withDefault(new C2PassthroughEnable::input(0))
                .withFields({C2F(mPassthroughEnable, value).any()})
                .withSetter(Setter<decltype(*mPassthroughEnable)>::StrictValueWithNoDeps)
                .build());
    }

    u_int32_t getMaxChannelCount() const { return mMaxChannelCount->value; }
    int32_t getPassthroughEnable() const { return mPassthroughEnable->value; }

private:
    std::shared_ptr<C2StreamSampleRateInfo::output> mSampleRate;
    std::shared_ptr<C2StreamChannelCountInfo::output> mChannelCount;
    std::shared_ptr<C2StreamBitrateInfo::input> mBitrate;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mInputMaxBufSize;
    std::shared_ptr<C2StreamMaxChannelCountInfo::input> mMaxChannelCount;
    std::shared_ptr<C2StreamChannelMaskInfo::output> mChannelMask;
    std::shared_ptr<C2PassthroughEnable::input> mPassthroughEnable;
};

C2AudioAC4Decoder::C2AudioAC4Decoder(
        const char *name,
        c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl)
    : C2AudioDecComponent(std::make_shared<AudioDecInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl),

    //add by amlogic
    mInputFlushing(false),
    mOutputFlushing(false),
    mInputFlushDone(false),
    mOutputFlushDone(false),
    mRunning(0),
    mIsFirst(0),
    mAbortPlaying(0),
    nPassThroughEnable(0),
    decode_offset(0),
    adec_call(false),
    mIsDualInput(false),
    mInBufSize(0),
    mOutPCMFrameSize(0),
    mMaxPCMOutBufSize(AC4_OUT_BUFFER_SIZE)
{

    C2AUDIO_LOGV("%s() %d  name:%s", __func__, __LINE__, name);
    {
        AutoMutex l(mSetUpLock);
        initializeState_l();
    }
    mAC4DecHandle = NULL;
    gAc4DecoderLibHandler = NULL;
    memset(&pcm_out_info, 0, sizeof(pcm_out_info));
    mOutputBuffer = (char *)malloc(AC4_OUT_BUFFER_SIZE);
    if (mOutputBuffer == NULL) {
        C2AUDIO_LOGE("%s() ac4 out pcm buffer malloc fail", __func__);
    } else {
        memset(mOutputBuffer, 0, AC4_OUT_BUFFER_SIZE);
    }
    ac4_decoder_init = NULL;
    ac4_decoder_cleanup = NULL;
    ac4_decoder_process = NULL;
    ac4_decoder_config = NULL;
}

C2AudioAC4Decoder::~C2AudioAC4Decoder() {
    onRelease();
    if (mOutputBuffer != NULL) {
        free(mOutputBuffer);
        mOutputBuffer = NULL;
    }
}

bool C2AudioAC4Decoder::tearDown() {
    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);
    AutoMutex l(mSetUpLock);
    if (mSetUp) {
        tearDownAudioDecoder_l();
    }
    return true;
}

void C2AudioAC4Decoder::initializeState_l() {
    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);
    {
        AutoMutex l(mConfigLock);
        mConfig = (AC4DecoderExternal *)malloc(sizeof(AC4DecoderExternal));
        if (mConfig == NULL) {
            C2AUDIO_LOGE("AC4DecoderExternal malloc err");
            return ;
        } else {
            memset(mConfig, 0, sizeof(AC4DecoderExternal));
        }
    }
    mEos = false;
    mSetUp = false;
}

c2_status_t C2AudioAC4Decoder::onInit() {
    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);

    status_t err = initDecoder();

    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);
    return err == OK ? C2_OK : C2_CORRUPTED;
}

c2_status_t C2AudioAC4Decoder::onStop() {
    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);

    mBuffersInfo.clear();
    {
        AutoMutex l(mFlushLock);
        mRunning = false;
    }
    mAbortPlaying = true;

    return C2_OK;
}

void C2AudioAC4Decoder::onRelease() {
    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);
    {
        AutoMutex l(mFlushLock);
        mRunning = false;
    }

    mInputFlushing = false;
    mOutputFlushing = false;
    mInputFlushDone = false;
    mOutputFlushDone = false;
    if (ac4_decoder_cleanup != NULL && mAC4DecHandle != NULL) {
        (*ac4_decoder_cleanup)(mAC4DecHandle);
        ac4_decoder_cleanup = NULL;
        mAC4DecHandle = NULL;
    }
    onStop();
    tearDown();
}

void C2AudioAC4Decoder::onReset() {
    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);
}

c2_status_t C2AudioAC4Decoder::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);

    return C2_OK;
}

c2_status_t C2AudioAC4Decoder::onFlush_sm() {
    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);
    mBuffersInfo.clear();

    return C2_OK;
}

c2_status_t C2AudioAC4Decoder::drainEos(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool,
        const std::unique_ptr<C2Work> &work) {
    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);
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

void C2AudioAC4Decoder::drainOutBuffer(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool,
        bool eos) {

    while (!mBuffersInfo.empty()) {
        Info &outInfo = mBuffersInfo.front();
        int numFrames = outInfo.decodedSizes.size();
        int outputDataSize =  mConfig->nBytesPCMOut;
        if (mConfig->debug_print) {
            C2AUDIO_LOGV("%s outputDataSize:%d,  outInfo numFrames:%d,frameIndex = %" PRIu64 "",__func__, outputDataSize, numFrames, outInfo.frameIndex);
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
                size_t bufferSize = mConfig->nBytesPCMOut;
                c2_status_t err = pool->fetchLinearBlock(bufferSize, usage, &block);
                if (err != C2_OK) {
                    C2AUDIO_LOGE("failed to fetch a linear block (%d)", err);
                    return std::bind(fillEmptyWork, _1, C2_NO_MEMORY);
                }
                C2WriteView wView = block->map().get();
                int16_t *outBuffer = reinterpret_cast<int16_t *>(wView.data());
                memcpy(outBuffer, mConfig->pOutputBuffer, mConfig->nBytesPCMOut);
                if (mConfig->debug_dump == 1) {
                    dump("/data/vendor/audiohal/c2_ac4_out.raw", (char *)outBuffer, mConfig->nBytesPCMOut);
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
        if (mConfig->debug_print) {
            /*coverity[use_after_free]*/
            C2AUDIO_LOGV("%s  mBuffersInfo is %s, out timestamp %" PRIu64 " / %u", __func__, mBuffersInfo.empty()?"null":"not null", outInfo.timestamp, block ? block->capacity() : 0);
        }
    }
}

void C2AudioAC4Decoder::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    // Initialize output work
    work->result = C2_OK;
    work->workletsProcessed = 1u;
    work->worklets.front()->output.configUpdate.clear();
    work->worklets.front()->output.flags = work->input.flags;

    int prevSampleRate = pcm_out_info.sample_rate;
    int prevNumChannels = pcm_out_info.channel_num;

    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;
    if (mConfig->debug_print) {
        C2AUDIO_LOGV("%s input.flags:0x%x  eos:%d", __func__, work->input.flags, eos);
    }

    uint8* inBuffer = NULL;
    C2ReadView view = mDummyReadView;
    size_t inBuffer_nFilledLen = 0u;
    if (!work->input.buffers.empty()) {
        view = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inBuffer_nFilledLen = view.capacity();
    }
    inBuffer = const_cast<uint8 *>(view.data());

    Info inInfo;
    inInfo.frameIndex = work->input.ordinal.frameIndex.peeku();
    inInfo.timestamp = work->input.ordinal.timestamp.peeku();
    inInfo.bufferSize = inBuffer_nFilledLen;
    inInfo.decodedSizes.clear();
    if (mConfig->debug_print) {
        C2AUDIO_LOGV("%s() inInfo.bufferSize:%zu, frameIndex:%" PRIu64 ", timestamp:%" PRIu64 "", __func__, inInfo.bufferSize, inInfo.frameIndex, inInfo.timestamp);
    }

    mConfig->pInputBuffer = inBuffer;
    mConfig->inputBufferCurrentLength = inBuffer_nFilledLen;
    mConfig->CurrentFrameLength = inBuffer_nFilledLen;
    mConfig->pOutputBuffer = (uint8 *)mOutputBuffer;
    mConfig->inputBufferUsedLength = 0;
    mConfig->nBytesPCMOut = 0;
    mConfig->nByteCurrentPCMOut = 0;

    if (mConfig->debug_dump == 1) {
        dump("/data/vendor/audiohal/c2_ac4_in.ac4", (char *)mConfig->pInputBuffer, mConfig->inputBufferCurrentLength);
    }

    int ret = 0;
    while (mConfig->inputBufferCurrentLength > 0) {
        int nBytesConsumed = 0;


        ret = (*ac4_decoder_process)(mAC4DecHandle
            , (const unsigned char *)(mConfig->pInputBuffer + mConfig->inputBufferUsedLength)
            , mConfig->inputBufferCurrentLength
            , (const unsigned char *)(mConfig->pOutputBuffer + mConfig->nBytesPCMOut)
            , &mConfig->nByteCurrentPCMOut
            , mMaxPCMOutBufSize
            , &pcm_out_info
            , &nBytesConsumed
            );

        if (ret) {
            C2AUDIO_LOGE("ATTENTION! ac4_decoder_process return error %d!\n", ret);
        }

        if (mConfig->debug_print) {
            C2AUDIO_LOGD("nbytes_consumed %d ret %d mConfig->inputBufferCurrentLength %d", nBytesConsumed, ret, mConfig->inputBufferCurrentLength);
        }

        if (mConfig->inputBufferCurrentLength >= nBytesConsumed) {
            mConfig->inputBufferUsedLength += nBytesConsumed;
            mConfig->inputBufferCurrentLength -= nBytesConsumed;
        } else {
            mConfig->inputBufferUsedLength += nBytesConsumed;
            mConfig->inputBufferCurrentLength = 0;
            break;
        }

        mConfig->num_channels = pcm_out_info.channel_num;
        mConfig->samplingRate = pcm_out_info.sample_rate;

        if (mConfig->nByteCurrentPCMOut > 0) {
            mConfig->nBytesPCMOut += mConfig->nByteCurrentPCMOut;
        }
    }

    //update output config.
    if (mConfig->nBytesPCMOut > 0) {
        inInfo.decodedSizes.push_back(mConfig->nBytesPCMOut);
        mBuffersInfo.push_back(std::move(inInfo));
    }
    if (!pcm_out_info.sample_rate || !pcm_out_info.channel_num) {
        C2AUDIO_LOGW("%s Invalid ac4 frame", __func__);
    } else if ((pcm_out_info.sample_rate != prevSampleRate) ||
               (pcm_out_info.channel_num != prevNumChannels)) {
        C2AUDIO_LOGI("Reconfiguring decoder: %d->%d Hz, %d->%d channels",
              prevSampleRate, pcm_out_info.sample_rate,
              prevNumChannels, pcm_out_info.channel_num);

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
            C2AUDIO_LOGE("Config Update failed");
            work->result = C2_CORRUPTED;
            return;
        }
    }
    if (eos) {
        drainEos(DRAIN_COMPONENT_WITH_EOS, pool, work);
    } else {
        drainOutBuffer(work, pool, false);
    }
}

bool C2AudioAC4Decoder::unload_ac4_decoder_lib() {
    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);
    if (ac4_decoder_cleanup != NULL && mAC4DecHandle != NULL) {
        (*ac4_decoder_cleanup)(mAC4DecHandle);
        ac4_decoder_cleanup = NULL;
        mAC4DecHandle = NULL;
    }
    ac4_decoder_init = NULL;
    ac4_decoder_process = NULL;
    ac4_decoder_cleanup = NULL;
    ac4_decoder_config = NULL;
    if (gAc4DecoderLibHandler != NULL) {
        dlclose(gAc4DecoderLibHandler);
        gAc4DecoderLibHandler = NULL;
    }
    return true;
}

bool C2AudioAC4Decoder::load_license_decoder_lib(const char *filename) {
    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);
    gAc4DecoderLibHandler = dlopen(filename, RTLD_NOW);
    if (!gAc4DecoderLibHandler) {
        C2AUDIO_LOGE("%s, failed to open (%s), %s\n", __FUNCTION__, filename, dlerror());
        goto Error;
    } else {
        C2AUDIO_LOGI("%s %d AC4 Decoder Lib(%s) Handler %p]", __FUNCTION__, __LINE__, filename, gAc4DecoderLibHandler);
    }

    ac4_decoder_init = (int (*)(int , int *, int *, int , void **))dlsym(gAc4DecoderLibHandler, "ac4_decoder_init");
    if (ac4_decoder_init == NULL) {
        C2AUDIO_LOGE("%s,can't find decoder lib,%s\n", __FUNCTION__, dlerror());
        goto Error;
    } else {
        C2AUDIO_LOGV("%s line %d ac4_decoder_init %p\n", __FUNCTION__, __LINE__, ac4_decoder_init);
    }

    ac4_decoder_process = (int (*)(void *, const unsigned char*, int , const unsigned char *, int *, int , struct pcm_info *, int *))dlsym(gAc4DecoderLibHandler, "ac4_decoder_process");
    if (ac4_decoder_process == NULL) {
        C2AUDIO_LOGE("%s,can't find decoder lib,%s\n", __FUNCTION__, dlerror());
        goto Error;
    } else {
        C2AUDIO_LOGV("%s line %d ac4_decoder_process %p\n", __FUNCTION__, __LINE__, ac4_decoder_process);
    }

    ac4_decoder_cleanup = (int (*)(void *))dlsym(gAc4DecoderLibHandler, "ac4_decoder_cleanup");
    if (ac4_decoder_cleanup == NULL) {
        C2AUDIO_LOGE("%s,can't find decoder lib,%s\n", __FUNCTION__, dlerror());
        goto Error;
    } else {
        C2AUDIO_LOGV("%s line %d ac4_decoder_cleanup %p\n", __FUNCTION__, __LINE__, ac4_decoder_cleanup);
    }

    ac4_decoder_config = (int (*)(void *, ac4_config_type_t, ac4_config_t *))dlsym(gAc4DecoderLibHandler, "ac4_decoder_config");
    if (ac4_decoder_config == NULL) {
        C2AUDIO_LOGE("%s,can't find decoder lib,%s\n", __FUNCTION__, dlerror());
    } else {
        C2AUDIO_LOGV("%s line %d ac4_decoder_config %p\n", __FUNCTION__, __LINE__, ac4_decoder_config);
    }

    return true;
Error:
    unload_ac4_decoder_lib();
    return false;
}

bool C2AudioAC4Decoder::load_ac4_decoder_lib() {
    bool ret = false;

    if (load_license_decoder_lib(DOLBY_MS12_V24_LIB_PATH)) {
        ret = true;
    }
    if (!ret && load_license_decoder_lib(DOLBY_MS12_V24_LIB_PATH_A)) {
        ret = true;
    }
    if (!ret && load_license_decoder_lib(DOLBY_MS12_V24_LIB_PATH_B)) {
        ret = true;
    }

    return ret;
}

bool C2AudioAC4Decoder::setUpAudioDecoder_l() {
    if (load_ac4_decoder_lib()) {
        char value[PROPERTY_VALUE_MAX];
        mConfig->debug_print = 0;
        mConfig->debug_dump = 0;
        memset(value, 0, sizeof(value));
        if ((property_get(C2_PROPERTY_AUDIO_DECODER_DEBUG,value,NULL) > 0) &&
            (!strcmp(value,"1") || !strcmp(value,"true")) ) {
            mConfig->debug_print = 1;
        }

        memset(value, 0, sizeof(value));
        if ((property_get(C2_PROPERTY_AUDIO_DECODER_DUMP,value,NULL) > 0) &&
            (!strcmp(value,"1") || !strcmp(value,"true")) ) {
            mConfig->debug_dump = 1;
        }
        propGetInt(CODEC2_ADEC_LOGDEBUG_PROPERTY, &gloglevel);
        C2AUDIO_LOGI("%s  debug_print:%d, debug_dump:%d,  gloglevel:%d", __func__, mConfig->debug_print, mConfig->debug_dump, gloglevel);

        int ret = (*ac4_decoder_init)(mIsDualInput, &mInBufSize, &mOutPCMFrameSize, mConfig->debug_print, &mAC4DecHandle);

        if (ret) {
            mIsFirst = true;
            C2AUDIO_LOGE("AC4 decoder initialization(handle return %p) failed!", mAC4DecHandle);
            return false;
        }
        else {
            C2AUDIO_LOGI("AC4 Decoder initialization(handle return %p) succeeded, got mInBufSize %d, mOutPCMFrameSize %d",
                gAc4DecoderLibHandler, mInBufSize, mOutPCMFrameSize);
            mIsFirst = true;
            return true;
        }

    }
    mIsFirst = true;
    return false;
}

bool C2AudioAC4Decoder::tearDownAudioDecoder_l() {
    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);
    if (mConfig != NULL) {
        free(mConfig);
        mConfig = NULL;
    }
    if (gAc4DecoderLibHandler != NULL)
        unload_ac4_decoder_lib();
    return true;
}

status_t C2AudioAC4Decoder::initDecoder() {
    status_t status = UNKNOWN_ERROR;

    C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);
    AutoMutex l(mSetUpLock);
    if (mSetUp) {
        C2AUDIO_LOGW("Trying to set up stream when you already have.");
        return OK;
    }
    if (!setUpAudioDecoder_l()) {
        C2AUDIO_LOGI("%s %d", __FUNCTION__, __LINE__);
        C2AUDIO_LOGE("setUpAC4AudioDecoder_l failed.");
        tearDownAudioDecoder_l();
        return C2_OMITTED;
    }
    C2AUDIO_LOGI("C2AudioAC4Decoder setUp done\n");
    mSetUp = true;

    status = OK;
    return status;
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

uint32_t C2AudioAC4Decoder::maskFromCount(uint32_t channelCount) {
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

class C2AudioAC4DecoderFactory : public C2ComponentFactory {
public:
    C2AudioAC4DecoderFactory(C2String decoderName) : mDecoderName(decoderName),
        mHelper(std::static_pointer_cast<C2ReflectorHelper>(
            GetCodec2VendorComponentStore()->getParamReflector())) {
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
            UNUSED(deleter);
        *component = std::shared_ptr<C2Component>(
                new C2AudioAC4Decoder(mDecoderName.c_str(),
                              id,
                              std::make_shared<C2AudioAC4Decoder::IntfImpl>(mHelper)));
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id, std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
            UNUSED(deleter);
        *interface = std::shared_ptr<C2ComponentInterface>(
                new AudioDecInterface<C2AudioAC4Decoder::IntfImpl>(
                        mDecoderName.c_str(), id, std::make_shared<C2AudioAC4Decoder::IntfImpl>(mHelper)));
        return C2_OK;
    }

    virtual ~C2AudioAC4DecoderFactory() override = default;

private:
    const C2String mDecoderName;
    std::shared_ptr<C2ReflectorHelper> mHelper;
};

} // namespace android

#define CreateC2AudioDecFactory(type) \
    extern "C" ::C2ComponentFactory* CreateC2AudioDecoder##type##Factory() {\
         ALOGV("create component %s ", #type);\
         return new ::android::C2AudioAC4DecoderFactory(COMPONENT_NAME_AC4);\
    }

#define DestroyC2AudioDecFactory(type) \
    extern "C" void DestroyC2AudioDecoder##type##Factory(::C2ComponentFactory* factory) {\
        delete factory;\
    }

CreateC2AudioDecFactory(AC4)
DestroyC2AudioDecFactory(AC4)

