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
#define LOG_TAG "Amlogic_C2AudioDTSDecoder"
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
#include "C2AudioDTSDecoder.h"
#include "aml_dts_decoder_api.h"
#include "C2VendorConfig.h"

#define LOGE ALOGE
#define LOGI ALOGI
#define LOGW ALOGW
#define LOGD ALOGD
#define LOGV ALOGV
#define LOG_LINE() ALOGD("[%s:%d]", __FUNCTION__, __LINE__);

#define UNUSED(expr)  \
    do {              \
        (void)(expr); \
    } while (0)

#define DTS_MAX_CHANNEL_COUNT   (8)
#define DTS_MAX_FRAME_LENGTH    (32768)

#define DTS_OUT_BUFFER_SIZE     (DTS_MAX_FRAME_LENGTH * 2)
#define DTS_IN_BUFFER_SIZE      (DTS_MAX_FRAME_LENGTH * 2)

#define AMADEC_CALL_OFFSET 6

constexpr char COMPONENT_NAME_DTS[] = "c2.amlogic.audio.decoder.dts";
constexpr char COMPONENT_NAME_DTSE[] = "c2.amlogic.audio.decoder.dtse";
constexpr char COMPONENT_NAME_DTSHD[] = "c2.amlogic.audio.decoder.dtshd";

#define DTS_HD_M6_LIB_PATH_A    "libHwAudio_dtshd.so"

namespace android {

static const char *ConvertComponentRoleToMimeType(const char *componentRole) {
    if (componentRole == NULL) {
        ALOGE("ConvertComponentRoleToMime componentRole is NULL!");
        return "NA";
    }
    // FIXME: 09/21: Android T was define MEDIA_MIMETYPE_AUDIO_DTS_HD
    if (strstr(componentRole, "dts")) {
        return const_cast<char *>(MEDIA_MIMETYPE_AUDIO_DTS);
    } else if (strstr(componentRole, "dtshd")) {
        return const_cast<char *>(MEDIA_MIMETYPE_AUDIO_DTS_HD);
        //return const_cast<char *>(MEDIA_MIMETYPE_AUDIO_DTS);
    } else if (strstr(componentRole, "dtse")) {
        return const_cast<char *>("audio/vnd.dts.hd;profile=lbr");
        //return const_cast<char *>(MEDIA_MIMETYPE_AUDIO_DTS);
    } else {
        ALOGE("Not support %s yet, need to add!", componentRole);
        return "NA";
    }
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
    }else
        ALOGE("error: open file failed\n");
}

class C2AudioDTSDecoder::IntfImpl : public AudioDecInterface<void>::BaseParams {
public:
    explicit IntfImpl(const char *name, const std::shared_ptr<C2ReflectorHelper> &helper)
        : AudioDecInterface<void>::BaseParams(
                helper,
                name,
                C2Component::KIND_DECODER,
                C2Component::DOMAIN_AUDIO,
                ConvertComponentRoleToMimeType(name)) {

        addParameter(
                DefineParam(mInputMediaType, C2_PARAMKEY_INPUT_MEDIA_TYPE)
                .withConstValue(AllocSharedString<C2PortMediaTypeSetting::input>(ConvertComponentRoleToMimeType(name)))
                .build());

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
                .withFields({C2F(mChannelCount, value).inRange(1, DTS_MAX_CHANNEL_COUNT)})
                .withSetter(Setter<decltype(*mChannelCount)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mMaxChannelCount, C2_PARAMKEY_MAX_CHANNEL_COUNT)
                .withDefault(new C2StreamMaxChannelCountInfo::input(0u, DTS_MAX_CHANNEL_COUNT))
                .withFields({C2F(mMaxChannelCount, value).inRange(1, DTS_MAX_CHANNEL_COUNT)})
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
                .withConstValue(new C2StreamMaxBufferSizeInfo::input(0u, DTS_MAX_FRAME_LENGTH))
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

C2AudioDTSDecoder::C2AudioDTSDecoder(
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
    nPassThroughEnable(0),
    decode_offset(0),
    adec_call(false)
{

    ALOGV("%s() %d  name:%s", __func__, __LINE__, name);
    {
        AutoMutex l(mSetUpLock);
        initializeState_l();
    }

    gDtsDecoderLibHandler = NULL;
    memset(&pcm_out_info, 0, sizeof(pcm_out_info));
    mOutputBuffer = (char *)malloc(DTS_OUT_BUFFER_SIZE);
    mOutputRawBuffer = (char *)malloc(DTS_OUT_BUFFER_SIZE);
    if (mOutputBuffer == NULL || mOutputRawBuffer == NULL) {
        ALOGE("%s() dts pcm/raw buffer malloc fail", __func__);
    } else {
        memset(mOutputBuffer, 0, DTS_OUT_BUFFER_SIZE);
        memset(mOutputRawBuffer, 0, DTS_OUT_BUFFER_SIZE);
    }
}

C2AudioDTSDecoder::~C2AudioDTSDecoder() {
    ALOGV("%s() %d", __func__, __LINE__);
    onRelease();

    if (mConfig != NULL) {
        if (mConfig->poutput_raw != NULL) {
            free(mConfig->poutput_raw);
            mConfig->poutput_raw = NULL;
        }
        free(mConfig);
        mConfig = NULL;
    }

    LOGV("%s() %d  exit", __func__, __LINE__);
}

bool C2AudioDTSDecoder::tearDown() {
    LOG_LINE();
    AutoMutex l(mSetUpLock);
    if (mSetUp) {
        tearDownAudioDecoder_l();
    }
    return true;
}

void C2AudioDTSDecoder::initializeState_l() {
    LOG_LINE();
    {
        AutoMutex l(mConfigLock);
        mConfig = (DTSDecoderExternal *)malloc(sizeof(DTSDecoderExternal));
        if (mConfig == NULL) {
            ALOGE("DTSDecoderExternal malloc err");
            return ;
        } else {
            memset(mConfig, 0, sizeof(DTSDecoderExternal));
            // FIXME: If digital_raw == 2, need to enlarge the poutput_raw size(For HBR 4XI2S).
            mConfig->channels =2;
            mConfig->samplerate = 48000;
        }
    }
    mEos = false;
    mSetUp = false;
}

c2_status_t C2AudioDTSDecoder::onInit() {
    LOG_LINE();

    status_t err = initDecoder();

    LOG_LINE();
    return err == OK ? C2_OK : C2_CORRUPTED;
}

c2_status_t C2AudioDTSDecoder::onStop() {
    LOG_LINE();

    mBuffersInfo.clear();
    {
        AutoMutex l(mFlushLock);
        mRunning = false;
    }
    mAbortPlaying = true;

    return C2_OK;
}

void C2AudioDTSDecoder::onRelease() {
    LOG_LINE();
    {
        AutoMutex l(mFlushLock);
        mRunning = false;
    }

    mInputFlushing = false;
    mOutputFlushing = false;
    mInputFlushDone = false;
    mOutputFlushDone = false;
    if (dts_decoder_cleanup != NULL) {
        (*dts_decoder_cleanup)();
        dts_decoder_cleanup = NULL;
    }
    onStop();
    tearDown();
}

void C2AudioDTSDecoder::onReset() {
    LOG_LINE();
}

c2_status_t C2AudioDTSDecoder::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    LOG_LINE();

    return C2_OK;
}

c2_status_t C2AudioDTSDecoder::onFlush_sm() {
    LOG_LINE();
    mBuffersInfo.clear();

    return C2_OK;
}

c2_status_t C2AudioDTSDecoder::drainEos(
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

void C2AudioDTSDecoder::drainOutBuffer(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool,
        bool eos) {

    while (!mBuffersInfo.empty()) {
        Info &outInfo = mBuffersInfo.front();
        int numFrames = outInfo.decodedSizes.size();
        int outputDataSize =  mConfig->outputlen;
        if (mConfig->debug_print) {
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
                size_t bufferSize = mConfig->outputlen;
                c2_status_t err = pool->fetchLinearBlock(bufferSize, usage, &block);
                if (err != C2_OK) {
                    ALOGE("failed to fetch a linear block (%d)", err);
                    return std::bind(fillEmptyWork, _1, C2_NO_MEMORY);
                }
                C2WriteView wView = block->map().get();
                int16_t *outBuffer = reinterpret_cast<int16_t *>(wView.data());
                memcpy(outBuffer, mConfig->pOutput, mConfig->outputlen);
                if (mConfig->debug_dump == 1) {
                    dump("/data/vendor/audiohal/c2_dts_out.raw", (char *)outBuffer, mConfig->outputlen);
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
            ALOGV("%s  mBuffersInfo is %s, out timestamp %" PRIu64 " / %u", __func__, mBuffersInfo.empty()?"null":"not null", outInfo.timestamp, block ? block->capacity() : 0);
        }
    }
}

void C2AudioDTSDecoder::process(
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
        ALOGV("%s input.flags:0x%x  eos:%d", __func__, work->input.flags, eos);
    }

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

    Info inInfo;
    inInfo.frameIndex = work->input.ordinal.frameIndex.peeku();
    inInfo.timestamp = work->input.ordinal.timestamp.peeku();
    inInfo.bufferSize = inBuffer_nFilledLen;
    inInfo.decodedSizes.clear();
    if (mConfig->debug_print) {
        ALOGV("%s() inInfo.bufferSize:%zu, frameIndex:%" PRIu64 ", timestamp:%" PRIu64 "", __func__, inInfo.bufferSize, inInfo.frameIndex, inInfo.timestamp);
    }

    if (inBuffer_nFilledLen) {
        mConfig->outputlen_frames = 0;
        decode_offset += inBuffer_nFilledLen;
    }

    mConfig->pInput = (char *)inBuffer;
    mConfig->inputlen = inBuffer_nFilledLen;
    mConfig->inputlen_used = 0;
    mConfig->poutput_pcm = mOutputBuffer;
    mConfig->poutput_raw = mOutputRawBuffer;
    mConfig->outputlen_pcm = 0;
    mConfig->outputlen_raw = 0;

    if (mConfig->debug_dump == 1) {
        dump("/data/vendor/audiohal/c2_dts_in.dts", mConfig->pInput, mConfig->inputlen);
    }

    if (mConfig->digital_raw < 3) {
        (*dts_decoder_process)((char *)mConfig->pInput
                                        ,mConfig->inputlen
                                        ,&mConfig->inputlen_used
                                        ,(char *)mConfig->poutput_pcm
                                        ,&mConfig->outputlen_pcm
                                        ,(struct pcm_info *)&pcm_out_info
                                        ,(char *)mConfig->poutput_raw
                                        ,(int *)&mConfig->outputlen_raw);
        if (mConfig->debug_print) {
            ALOGV("inputlen:%d inputlen_used:%d outputlen_pcm:%d outputlen_raw:%d (sr:%d ch:%d bw:%d)",
                mConfig->inputlen, mConfig->inputlen_used, mConfig->outputlen_pcm, mConfig->outputlen_raw,
                pcm_out_info.sample_rate, pcm_out_info.channel_num, pcm_out_info.bytes_per_sample);
        }
        if ((pcm_out_info.sample_rate > 0 &&
            pcm_out_info.sample_rate <= 192000 &&
            pcm_out_info.sample_rate != mConfig->samplerate) ||
                (pcm_out_info.channel_num > 0 &&
                pcm_out_info.channel_num <=8 &&
                pcm_out_info.channel_num != mConfig->channels)) {
            ALOGI("decoder sample rate changed from %d to %d ,ch num changed from %d to %d ",
                mConfig->samplerate, pcm_out_info.sample_rate,mConfig->channels,pcm_out_info.channel_num);
            mConfig->samplerate = pcm_out_info.sample_rate;
            mConfig->channels = pcm_out_info.channel_num;
        }

        if (mConfig->digital_raw > 0) {
            mConfig->pOutput = mConfig->poutput_raw;
            mConfig->outputlen = mConfig->outputlen_raw;
        } else {
            mConfig->pOutput = mConfig->poutput_pcm;
            mConfig->outputlen = mConfig->outputlen_pcm;
        }
        mConfig->outputlen_frames = mConfig->outputlen / mConfig->channels;//total decoded frames
    } else {
        // This case is designed for Xiaomi. When digital_raw=3,
        // omx will bypass audio es, and the player reads back the audio es,
        // and send to audioflinger in offload mode.
        memcpy((char *)mConfig->poutput_pcm, (char *)mConfig->pInput, mConfig->inputlen);
        mConfig->pOutput = mConfig->poutput_pcm;
        mConfig->outputlen = mConfig->inputlen;
        pcm_out_info.bitstream_type = 1;
        pcm_out_info.bytes_per_sample = 2;
        pcm_out_info.channel_num = 2;
        pcm_out_info.sample_rate = 48000;
    }

    //update output config.
    if (mConfig->outputlen > 0) {
        inInfo.decodedSizes.push_back(mConfig->outputlen);
        mBuffersInfo.push_back(std::move(inInfo));
    }
    if (!pcm_out_info.sample_rate || !pcm_out_info.channel_num) {
        ALOGW("%s Invalid dts frame", __func__);
    } else if ((pcm_out_info.sample_rate != prevSampleRate) ||
               (pcm_out_info.channel_num != prevNumChannels)) {
        ALOGI("Reconfiguring decoder: %d->%d Hz, %d->%d channels",
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
            ALOGE("Config Update failed");
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

bool C2AudioDTSDecoder::unload_dts_decoder_lib(){
    LOG_LINE();
    if (dts_decoder_cleanup != NULL) {
        (*dts_decoder_cleanup)();
        dts_decoder_cleanup =NULL;
    }
    dts_decoder_init = NULL;
    dts_decoder_process = NULL;
    dts_decoder_cleanup = NULL;
    if (gDtsDecoderLibHandler != NULL) {
        dlclose(gDtsDecoderLibHandler);
        gDtsDecoderLibHandler = NULL;
    }
    return true;
}

bool C2AudioDTSDecoder::load_dts_decoder_lib(const char *filename){
    LOG_LINE();
    gDtsDecoderLibHandler = dlopen(filename, RTLD_NOW);
    if (!gDtsDecoderLibHandler) {
        ALOGE("%s, failed to open (%s), %s\n", __FUNCTION__,filename,  dlerror());
        goto Error;
    } else {
        ALOGV("<%s::%d>--[get lib handler]", __FUNCTION__, __LINE__);
    }
    dts_decoder_init = (int (*)(int, int))dlsym(gDtsDecoderLibHandler, "dca_decoder_init");
    if (dts_decoder_init == NULL) {
        ALOGE("%s,err to find %s\n", __FUNCTION__, dlerror());
        goto Error;
    }
    dts_decoder_process = (int (*)(char * ,int ,int *,char *,int *,struct pcm_info *,char *,int *))dlsym(gDtsDecoderLibHandler, "dca_decoder_process");
    if (dts_decoder_process == NULL) {
        ALOGE("%s,err to find %s\n", __FUNCTION__, dlerror());
        goto Error;
    }
    dts_decoder_cleanup = (int (*)())dlsym(gDtsDecoderLibHandler, "dca_decoder_deinit");
    if (dts_decoder_cleanup == NULL) {
        ALOGE("%s,err to find %s\n", __FUNCTION__, dlerror());
        goto Error;
    }
    return true;
Error:
    unload_dts_decoder_lib();
    return false;
}

bool C2AudioDTSDecoder::setUpAudioDecoder_l() {
    if (load_dts_decoder_lib(DTS_HD_M6_LIB_PATH_A)) {
        char value[PROPERTY_VALUE_MAX];
        mConfig->debug_print = 0;
        mConfig->debug_dump = 0;
        memset(value, 0, sizeof(value));
        if ((property_get("vendor.media.c2.audio.debug", value, NULL) > 0) &&
            (!strcmp(value,"1") || !strcmp(value,"true")) ) {
            mConfig->debug_print = 1;
        }

        memset(value, 0, sizeof(value));
        if ((property_get("vendor.media.c2.audio.dump", value, NULL) > 0) &&
            (!strcmp(value,"1") || !strcmp(value,"true")) ) {
            mConfig->debug_dump = 1;
        }

        nPassThroughEnable = mIntf->getPassthroughEnable();
        if (nPassThroughEnable >= AMADEC_CALL_OFFSET) {
            mConfig->digital_raw = nPassThroughEnable - AMADEC_CALL_OFFSET;
            adec_call = true;
        } else  {
            mConfig->digital_raw = nPassThroughEnable;
            adec_call = false;
        }
        ALOGI("mConfig->digital_raw:%d ", mConfig->digital_raw);
        // When digital_raw=3, omx will bypass audio es. No need to init decoder.
        if (mConfig->digital_raw != 3) {
            dts_decoder_init(1,mConfig->digital_raw);
        }
        mIsFirst = true;
        return true;
    }
    mIsFirst = true;
    return false;
}

bool C2AudioDTSDecoder::tearDownAudioDecoder_l() {
    LOG_LINE();
    if (mConfig != NULL) {
        if (mConfig->poutput_raw != NULL) {
            free(mConfig->poutput_raw);
            mConfig->poutput_raw = NULL;
        }
        free(mConfig);
        mConfig = NULL;
    }
    if (gDtsDecoderLibHandler != NULL)
        unload_dts_decoder_lib();
    return true;
}

bool C2AudioDTSDecoder::isSetUp() {
    AutoMutex l(mSetUpLock);
    return mSetUp;
}

status_t C2AudioDTSDecoder::initDecoder() {
    status_t status = UNKNOWN_ERROR;

    LOG_LINE();
    AutoMutex l(mSetUpLock);
    if (mSetUp) {
        ALOGW("Trying to set up stream when you already have.");
        return OK;
    }
    if (!setUpAudioDecoder_l()) {
        LOG_LINE();
        ALOGE("setUpDTSAudioDecoder_l failed.");
        tearDownAudioDecoder_l();
        return C2_OMITTED;
    }
    ALOGI("C2AudioDTSDecoder setUp done\n");
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

uint32_t C2AudioDTSDecoder::maskFromCount(uint32_t channelCount) {
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

class C2AudioDTSDecoderFactory : public C2ComponentFactory {
public:
    C2AudioDTSDecoderFactory(C2String decoderName) : mDecoderName(decoderName),
        mHelper(std::static_pointer_cast<C2ReflectorHelper>(
            GetCodec2VendorComponentStore()->getParamReflector())) {
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
            UNUSED(deleter);
            ALOGI("in %s, mDecoderName:%s", __func__, mDecoderName.c_str());
        *component = std::shared_ptr<C2Component>(
                new C2AudioDTSDecoder(mDecoderName.c_str(),
                              id,
                              std::make_shared<C2AudioDTSDecoder::IntfImpl>(mDecoderName.c_str(), mHelper)));
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id, std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
            //ALOGI("in %s, id:%d,  start to create C2ComponentInterface", __func__, id);
            UNUSED(deleter);
        *interface = std::shared_ptr<C2ComponentInterface>(
                new AudioDecInterface<C2AudioDTSDecoder::IntfImpl>(
                        mDecoderName.c_str(), id, std::make_shared<C2AudioDTSDecoder::IntfImpl>(mDecoderName.c_str(), mHelper)));
        return C2_OK;
    }

    virtual ~C2AudioDTSDecoderFactory() override = default;

private:
    const C2String mDecoderName;
    std::shared_ptr<C2ReflectorHelper> mHelper;
};


}  // namespace android

#define CreateC2AudioDecFactory(type) \
    extern "C" ::C2ComponentFactory* CreateC2AudioDecoder##type##Factory() { \
         ALOGV("create component %s ", #type); \
         std::string type_dtse = "DTSE"; \
         std::string type_dtshd = "DTSHD"; \
         std::string type_dts = "DTS"; \
         if (!type_dtshd.compare(#type)) { \
            return new ::android::C2AudioDTSDecoderFactory(COMPONENT_NAME_DTSHD); \
         } else if (!type_dtse.compare(#type)) { \
            return new ::android::C2AudioDTSDecoderFactory(COMPONENT_NAME_DTSE); \
         } else { \
            return new ::android::C2AudioDTSDecoderFactory(COMPONENT_NAME_DTS); \
         } \
    }

#define DestroyC2AudioDecFactory(type) \
    extern "C" void DestroyC2AudioDecoder##type##Factory(::C2ComponentFactory* factory) {\
        delete factory;\
    }

CreateC2AudioDecFactory(DTS)
DestroyC2AudioDecFactory(DTS)
CreateC2AudioDecFactory(DTSE)
DestroyC2AudioDecFactory(DTSE)
CreateC2AudioDecFactory(DTSHD)
DestroyC2AudioDecFactory(DTSHD)

