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
#define LOG_TAG "Amlogic_C2AudioDTSXDecoder"
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
#include "C2AudioDTSXDecoder.h"
#include "aml_dtsx_decoder_api.h"
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

#define DTSX_MAX_CHANNELS   (12)
#define DTSX_MAX_FRAME_LENGTH    (65536)

#define DTSX_WAV_FILE_SCRATCH_PAD_SIZE_IN_SAMPLES    (256)
#define DTSX_WAV_FILE_BYTES_PER_SAMPLE_24                    (3)
#define AUDIO_MAX_SUB_FRAMES                                            8
#define AUDIO_MAX_SUB_SUB_FRAMES                                    4

#define DTSX_OUT_BUFFER_SIZE    ((DTSX_MAX_CHANNELS * \
                                    DTSX_WAV_FILE_SCRATCH_PAD_SIZE_IN_SAMPLES * \
                                    DTSX_WAV_FILE_BYTES_PER_SAMPLE_24 * \
                                    AUDIO_MAX_SUB_FRAMES * \
                                    AUDIO_MAX_SUB_SUB_FRAMES \
                                    + 32 * 1024 /*used for transcoder */ \
                                    + 8) * 2)

#define AMADEC_CALL_OFFSET 6

constexpr char COMPONENT_NAME_DTS[] = "c2.amlogic.audio.decoder.dts";
constexpr char COMPONENT_NAME_DTSE[] = "c2.amlogic.audio.decoder.dtse";
constexpr char COMPONENT_NAME_DTSHD[] = "c2.amlogic.audio.decoder.dtshd";
constexpr char COMPONENT_NAME_DTSUHD[] = "c2.amlogic.audio.decoder.dtsuhd";

typedef enum {
    DTSX_OUTPUT_SPK = 0,    /**< Multi-channel Bus 0 (for feeding the Virtual-X / Headphone-X processor) */
    DTSX_OUTPUT_RAW = 1,    /**< Multi-channel Bus 1 (for feeding transcoder) */
    DTSX_OUTPUT_HP  = 2,    /**< Stereo downmix Bus (downmix from Multi-channel Bus 1) */
    DTSX_OUTPUT_MAX = 3
} AML_DTSX_OUTPUT_TYPE;

typedef struct dtsx_config_params_s {
    int core1_dec_out;
    int core2_spkr_out;// core2 bus0 output speaker mask, this setting will only take effect when auto_config_out_for_vx is set to -1
    int auto_config_out_for_vx;// Auto config output channel for Virtual X: -1:default without vx. 0:auto config output according to DecMode. 2:always downmix to stereo.
    int dec_sink_dev_type;
    int pp_sink_dev_type;
    bool bPassthrough;
    int limiter_type[DTSX_OUTPUT_MAX];
    bool drc_enable[DTSX_OUTPUT_MAX];
    int drc_profile[DTSX_OUTPUT_MAX];          // DTSX_DRC_PROFILE_E
    int drc_default_curve[DTSX_OUTPUT_MAX];    // DTSX_DRC_DEFAULT_CURVE_E
    int drc_cut_value[DTSX_OUTPUT_MAX];        // rang: 0 ~ 100
    int drc_boost_value[DTSX_OUTPUT_MAX];      // rang: 0 ~ 100
    bool loudness_enable[DTSX_OUTPUT_MAX];
    int loudness_target[DTSX_OUTPUT_MAX];      // rang: -60 ~ -10
    bool neuralx_up_mix;
    bool neox_down_mix;
} dtsx_config_params_t;

enum AML_DTSX_DRC_PROFILE
{
    DTSX2_DEC_DRC_PROFILE_LOW = 0,                            /**< DRC Low profile setting */
    DTSX2_DEC_DRC_PROFILE_MEDIUM = 1,                         /**< DRC Medium profile setting  */
    DTSX2_DEC_DRC_PROFILE_HIGH = 2,                           /**< DRC High profile setting */
    DTSX2_DEC_DRC_PROFILE_MAX = 3                             /**< This must always be the last entry on the list. Reserved for internal use only */
} DTSX_DRC_PROFILE_E;

enum AML_DTSX_DRC_DEFAULT_CURVE
{
    DTSX_DEC_DRC_DEFAULT_CURVE_NO_COMPRESSION = 0,            /**< No DRC Default Compression Curve */
    DTSX_DEC_DRC_DEFAULT_CURVE_LEGACY_FILM_STANDARD,          /**< DRC Default Compression Curve for Legacy Film Standard */
    DTSX_DEC_DRC_DEFAULT_CURVE_LEGACY_FILM_LIGHT,             /**< DRC Default Compression Curve for Legacy Film light */
    DTSX_DEC_DRC_DEFAULT_CURVE_LEGACY_MUSIC_STANDARD,         /**< DRC Default Compression Curve for Legacy Music Standard */
    DTSX_DEC_DRC_DEFAULT_CURVE_LEGACY_MUSIC_LIGHT,            /**< DRC Default Compression Curve for Legacy Music light */
    DTSX_DEC_DRC_DEFAULT_CURVE_LEGACY_SPEECH,                 /**< DRC Default Compression Curve for Legacy speech  */
    DTSX_DEC_DRC_DEFAULT_CURVE_COMMON1,                       /**< DRC Default Compression Curve for DTS:X Profile2 common1  */
    DTSX_DEC_DRC_DEFAULT_CURVE_COMMON2,                       /**< DRC Default Compression Curve for DTS:X Profile2 common2  */
    DTSX_DEC_DRC_DEFAULT_CURVE_COMMON3,                       /**< DRC Default Compression Curve for DTS:X Profile2 common3  */
    DTSX_DEC_DRC_DEFAULT_CURVE_COMMON4,                       /**< DRC Default Compression Curve for DTS:X Profile2 common4  */
    DTSX_DEC_DRC_DEFAULT_CURVE_COMMON5,                       /**< DRC Default Compression Curve for DTS:X Profile2 common5  */
    DTSX_DEC_DRC_DEFAULT_CURVE_COMMON6,                       /**< DRC Default Compression Curve for DTS:X Profile2 common6  */
    DTSX_DEC_DRC_DEFAULT_CURVE_COMMON7,                       /**< DRC Default Compression Curve for DTS:X Profile2 common7  */
    DTSX_DEC_DRC_DEFAULT_CURVE_COMMON8,                       /**< DRC Default Compression Curve for DTS:X Profile2 common8  */
    DTSX_DEC_DRC_DEFAULT_CURVE_COMMON9,                       /**< DRC Default Compression Curve for DTS:X Profile2 common9  */
    DTSX_DEC_DRC_DEFAULT_CURVE_CUSTOM,                        /**< DRC Default Compression Curve for DTS:X Profile2 custom   */
    DTSX_DEC_DRC_DEFAULT_CURVE_PREDEFINED_MAX = DTSX_DEC_DRC_DEFAULT_CURVE_CUSTOM, /**< DRC Default Compression Curve for Pre-defined curve  */
    DTSX_DEC_DRC_DEFAULT_CURVE_MAX
} DTSX_DRC_DEFAULT_CURVE_E;

static const dtsx_config_params_t _dtsx_config_params = {
    .core1_dec_out = 2123,
    .core2_spkr_out = 2,
    .auto_config_out_for_vx = -1,
    .dec_sink_dev_type = 0,
    .pp_sink_dev_type = 0,
    .bPassthrough = 0,
    .limiter_type[DTSX_OUTPUT_SPK] = 0,
    .limiter_type[DTSX_OUTPUT_RAW] = 1,
    .limiter_type[DTSX_OUTPUT_HP] = 1,
    .drc_enable[DTSX_OUTPUT_SPK] = 1,
    .drc_enable[DTSX_OUTPUT_RAW] = 0,
    .drc_enable[DTSX_OUTPUT_HP] = 1,
    .drc_profile[DTSX_OUTPUT_SPK] = DTSX2_DEC_DRC_PROFILE_MEDIUM,
    .drc_profile[DTSX_OUTPUT_RAW] = DTSX2_DEC_DRC_PROFILE_MEDIUM,
    .drc_profile[DTSX_OUTPUT_HP] =  DTSX2_DEC_DRC_PROFILE_MEDIUM,
    .drc_default_curve[DTSX_OUTPUT_SPK] = DTSX_DEC_DRC_DEFAULT_CURVE_LEGACY_FILM_LIGHT,
    .drc_default_curve[DTSX_OUTPUT_RAW] = DTSX_DEC_DRC_DEFAULT_CURVE_NO_COMPRESSION,
    .drc_default_curve[DTSX_OUTPUT_HP] =  DTSX_DEC_DRC_DEFAULT_CURVE_LEGACY_FILM_LIGHT,
    .drc_cut_value[DTSX_OUTPUT_SPK] = 100,
    .drc_cut_value[DTSX_OUTPUT_RAW] = 100,
    .drc_cut_value[DTSX_OUTPUT_HP] = 100,
    .drc_boost_value[DTSX_OUTPUT_SPK] = 100,
    .drc_boost_value[DTSX_OUTPUT_RAW] = 100,
    .drc_boost_value[DTSX_OUTPUT_HP] = 100,
    .loudness_enable[DTSX_OUTPUT_SPK] = 1,
    .loudness_enable[DTSX_OUTPUT_RAW] = 0,
    .loudness_enable[DTSX_OUTPUT_HP] = 1,
    .loudness_target[DTSX_OUTPUT_SPK] = -20,
    .loudness_target[DTSX_OUTPUT_RAW] = -31,
    .loudness_target[DTSX_OUTPUT_HP] = -31,
    .neuralx_up_mix = 1,
    .neox_down_mix = 1
};
namespace android {

static const char *ConvertComponentRoleToMimeType(const char *componentRole) {
    if (componentRole == NULL) {
        ALOGE("ConvertComponentRoleToMime componentRole is NULL!");
        return "NA";
    }
    if (strstr(componentRole, "dts")) {
        return const_cast<char *>(MEDIA_MIMETYPE_AUDIO_DTS);
    } else if (strstr(componentRole, "dtshd")) {
        return const_cast<char *>(MEDIA_MIMETYPE_AUDIO_DTS_HD);
    } else if (strstr(componentRole, "dtse")) {
        return const_cast<char *>("audio/vnd.dts.hd;profile=lbr");
    } else if(strstr(componentRole, "dtsuhd")) {
        return const_cast<char*>("audio/vnd.dts.uhd;profile=p2");
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

class C2AudioDTSXDecoder::IntfImpl : public AudioDecInterface<void>::BaseParams {
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
                .withFields({C2F(mChannelCount, value).inRange(1, DTSX_MAX_CHANNELS)})
                .withSetter(Setter<decltype(*mChannelCount)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mMaxChannelCount, C2_PARAMKEY_MAX_CHANNEL_COUNT)
                .withDefault(new C2StreamMaxChannelCountInfo::input(0u, DTSX_MAX_CHANNELS))
                .withFields({C2F(mMaxChannelCount, value).inRange(1, DTSX_MAX_CHANNELS)})
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
                .withConstValue(new C2StreamMaxBufferSizeInfo::input(0u, DTSX_MAX_FRAME_LENGTH))
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

C2AudioDTSXDecoder::C2AudioDTSXDecoder(
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
    mIsFirst(false),
    mAbortPlaying(false),
    nPassThroughEnable(0),
    decode_offset(0),
    adec_call(false)
{

    ALOGV("%s() %d  name:%s", __func__, __LINE__, name);
    {
        AutoMutex l(mSetUpLock);
        initializeState_l();
    }

    gDtsxDecoderLibHandler  = nullptr;
    _aml_dts_decoder_init  = nullptr;
    _aml_dts_decoder_deinit  = nullptr;
    _aml_dts_decoder_process  = nullptr;
    _aml_dts_postprocess_init = nullptr;
    _aml_dts_decoder_get_output_info  = nullptr;
    _aml_dts_postprocess_deinit  = nullptr;
    _aml_dts_postprocess_proc  = nullptr;
    _aml_dts_metadata_update  = nullptr;
    _aml_dts_postprocess_get_out_info  = nullptr;
    _aml_dts_postprocess_dynamic_parameter_set  = nullptr;

    memset(&pcm_out_info, 0, sizeof(pcm_out_info));
    mOutputBuffer = (char *)malloc(DTSX_OUT_BUFFER_SIZE);
    if (mOutputBuffer == NULL) {
        ALOGE("%s() dts output buffer malloc fail", __func__);
    } else {
        memset(mOutputBuffer, 0, DTSX_OUT_BUFFER_SIZE);
    }
}

/*coverity[exn_spec_violation]*/
C2AudioDTSXDecoder::~C2AudioDTSXDecoder() {
    ALOGV("%s() %d", __func__, __LINE__);
    onRelease();

    if (mConfig != NULL) {
        if (mOutputBuffer != NULL) {
            free(mOutputBuffer);
            mOutputBuffer = NULL;
        }
        free(mConfig);
        mConfig = NULL;
    }

    LOGV("%s() %d  exit", __func__, __LINE__);
}
int C2AudioDTSXDecoder::_dtsx_pcm_output()
{
    int nSampleRate = 0;
    int nChannel = 0;
    int nBitWidth = 0;
    int ret = 0;
    ret = _aml_dts_postprocess_get_out_info(mConfig->p_dtsx_pp_inst, DTSX_OUTPUT_SPK, &nSampleRate, &nChannel, &nBitWidth);
    if (ret != 0) {
        ALOGE("[%s:%d] _aml_dts_postprocess_get_out_info fail:%d", __func__, __LINE__, ret);
    } else {
        if ((nSampleRate > 0 && nSampleRate <= 192000 && nSampleRate != mConfig->samplerate)
            || (nChannel > 0 && nChannel <= 8 && nChannel != mConfig->channels)) {
            ALOGI("decoder sample rate changed from %d to %d ,ch num changed from %d to %d ",
                mConfig->samplerate, nSampleRate, mConfig->channels, nChannel);
            mConfig->samplerate = nSampleRate;
            mConfig->channels = nChannel;
        }
    }
    // TODO: Check outHeader->pBuffer length.
    memcpy(mConfig->pOutput, mConfig->a_dtsx_pp_output[DTSX_OUTPUT_SPK], mConfig->a_dtsx_pp_output_size[DTSX_OUTPUT_SPK]);
    mConfig->outputlen = mConfig->a_dtsx_pp_output_size[DTSX_OUTPUT_SPK];

    if (mConfig->debug_dump == 1)
        dump("/data/tmp/omx_dtsx_audio_pcmout.pcm", mConfig->pOutput, mConfig->outputlen);
    if (mConfig->debug_print == 1) {
        ALOGD("pcm_spk_output: length:%d sr:%d ch:%d",
            mConfig->outputlen,
            mConfig->samplerate,
            mConfig->channels);
    }
    return 0;
}

int C2AudioDTSXDecoder::_dtsx_raw_output()
{
    int nSampleRate = 0;
    int nChannel = 0;
    int nBitWidth = 0;
    int ret = 0;

    ret = _aml_dts_postprocess_get_out_info(mConfig->p_dtsx_pp_inst, DTSX_OUTPUT_RAW, &nSampleRate, &nChannel, &nBitWidth);
    if (ret != 0) {
        ALOGE("[%s:%d] _aml_dts_postprocess_get_out_info fail:%d", __func__, __LINE__, ret);
    } else {
        if ((nSampleRate > 0 && nSampleRate <= 192000 && nSampleRate != mConfig->samplerate)
            || (nChannel > 0 && nChannel <= 8 && nChannel != mConfig->channels)) {
            ALOGI("decoder sample rate changed from %d to %d ,ch num changed from %d to %d ",
                mConfig->samplerate, nSampleRate, mConfig->channels, nChannel);
            mConfig->samplerate = nSampleRate;
            mConfig->channels = nChannel;
        }
    }

    // TODO: Check outHeader->pBuffer length.
    memcpy(mConfig->pOutput, mConfig->a_dtsx_pp_output[DTSX_OUTPUT_RAW], mConfig->a_dtsx_pp_output_size[DTSX_OUTPUT_RAW]);
    mConfig->outputlen = mConfig->a_dtsx_pp_output_size[DTSX_OUTPUT_RAW];

    if (mConfig->debug_dump == 1)
        dump("/data/tmp/omx_dtsx_audio_rawout.pcm", mConfig->pOutput, mConfig->outputlen);
    if (mConfig->debug_print == 1) {
        ALOGD("raw_output: length:%d sr:%d ch:%d",
            mConfig->outputlen,
            mConfig->samplerate,
            mConfig->channels);
    }
    return 0;
}
int C2AudioDTSXDecoder::_aml_dtsx_dualcore_init()
{
    LOG_LINE();
    int cmd_count = 0;
    int ret = 0;

    /* Prepare the init argv for core1 decoder */
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_core1_max_spkrout=%d", _dtsx_config_params.core1_dec_out);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_unalignedsyncword");
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_dec_sinkdevtype=%d", _dtsx_config_params.dec_sink_dev_type);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_passthrough_enable=%d", _dtsx_config_params.bPassthrough);

    ret = (_aml_dts_decoder_init)(&mConfig->p_dtsx_dec_inst, cmd_count, (const char **)(mConfig->init_argv));
    if (ret != 0) {
        ALOGE("_aml_dts_decoder_init fail:%d", ret);
        goto DTSX_DUALCORE_INIT_FAIL;
    }

    /* Prepare the init argv for core2 post processing */
    cmd_count = 0;
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_pp_sinkdevtype=%d", _dtsx_config_params.dec_sink_dev_type);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_core2_spkrout=%d", 2);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_config_output_for_vx=%d", _dtsx_config_params.auto_config_out_for_vx);

    // hybrid limiting in linked mode (Medium MIPS)
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_spk_limitertype=%d", _dtsx_config_params.limiter_type[DTSX_OUTPUT_SPK]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_transcoder_limitertype=%d", _dtsx_config_params.limiter_type[DTSX_OUTPUT_RAW]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_hp_limitertype=%d", _dtsx_config_params.limiter_type[DTSX_OUTPUT_HP]);

    // Range: [-60, -10]
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_spk_loudnesstarget=%d", _dtsx_config_params.loudness_target[DTSX_OUTPUT_SPK]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_transcoder_loudnesstarget=%d", _dtsx_config_params.loudness_target[DTSX_OUTPUT_RAW]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_hp_loudnesstarget=%d", _dtsx_config_params.loudness_target[DTSX_OUTPUT_HP]);

    // Loudness enable
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_spk_loudnessenable=%d", _dtsx_config_params.loudness_enable[DTSX_OUTPUT_SPK]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_transcoder_loudnessenable=%d", _dtsx_config_params.loudness_enable[DTSX_OUTPUT_RAW]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_hp_loudnessenable=%d", _dtsx_config_params.loudness_enable[DTSX_OUTPUT_HP]);

    // DRC Medium profile setting
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_spk_drcprofile=%d", _dtsx_config_params.drc_profile[DTSX_OUTPUT_SPK]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_transcoder_drcprofile=%d", _dtsx_config_params.drc_profile[DTSX_OUTPUT_RAW]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_hp_drcprofile=%d", _dtsx_config_params.drc_profile[DTSX_OUTPUT_HP]);

    // DRC Default Compression Curve for Legacy Music Light
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_spk_drcdefaultcurve=%d", _dtsx_config_params.drc_default_curve[DTSX_OUTPUT_SPK]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_transcoder_drcdefaultcurve=%d", _dtsx_config_params.drc_default_curve[DTSX_OUTPUT_RAW]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_hp_drcdefaultcurve=%d", _dtsx_config_params.drc_default_curve[DTSX_OUTPUT_HP]);

    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_spk_drccutvalue=%d", _dtsx_config_params.drc_cut_value[DTSX_OUTPUT_SPK]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_transcoder_drccutvalue=%d", _dtsx_config_params.drc_cut_value[DTSX_OUTPUT_RAW]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_hp_drccutvalue=%d", _dtsx_config_params.drc_cut_value[DTSX_OUTPUT_HP]);

    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_spk_drcboostvalue=%d", _dtsx_config_params.drc_boost_value[DTSX_OUTPUT_SPK]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_transcoder_drcboostvalue=%d", _dtsx_config_params.drc_boost_value[DTSX_OUTPUT_RAW]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_hp_drcboostvalue=%d", _dtsx_config_params.drc_boost_value[DTSX_OUTPUT_HP]);

    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_spk_drcenable=%d", _dtsx_config_params.drc_enable[DTSX_OUTPUT_SPK]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_transcoder_drcenable=%d", _dtsx_config_params.drc_enable[DTSX_OUTPUT_RAW]);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_hp_drcenable=%d", _dtsx_config_params.drc_enable[DTSX_OUTPUT_HP]);

    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_neuralxupmixenable=%d", _dtsx_config_params.neuralx_up_mix);
    snprintf(mConfig->init_argv[cmd_count++], DTSX_PARAM_STRING_LEN, "dtsx_neoxdownmixenable=%d", _dtsx_config_params.neox_down_mix);

    ret = (_aml_dts_postprocess_init)(&mConfig->p_dtsx_pp_inst, cmd_count, (const char **)(mConfig->init_argv));
    if (ret != 0) {
        ALOGE("_aml_dts_decoder_process fail:%d", ret);
        goto DTSX_DUALCORE_INIT_FAIL;
    }

    ALOGI("[%s:%d] out", __func__, __LINE__);
    return 0;

DTSX_DUALCORE_INIT_FAIL:
    if (mConfig->p_dtsx_dec_inst) {
        (_aml_dts_decoder_deinit)(mConfig->p_dtsx_dec_inst);
    }

    unload_dtsx_decoder_lib();
    return -1;
}

bool C2AudioDTSXDecoder::tearDown() {
    LOG_LINE();
    AutoMutex l(mSetUpLock);
    if (mSetUp) {
        tearDownAudioDecoder_l();
    }
    return true;
}

void C2AudioDTSXDecoder::initializeState_l() {
    LOG_LINE();
    {
        AutoMutex l(mConfigLock);
        mConfig = (DTSXDecoderExternal *)malloc(sizeof(DTSXDecoderExternal));
        if (mConfig == NULL) {
            ALOGE("malloc err");
            return ;
        } else {
            memset(mConfig, 0, sizeof(DTSXDecoderExternal));
            memset(&pcm_out_info, 0, sizeof(pcm_out_info));
            mConfig->core1_out_buff_size = DTSX_MAX_FRAME_LENGTH;
            mConfig->core1_out_pcm = (char *)malloc(mConfig->core1_out_buff_size);
            memset(mConfig->core1_out_pcm, 0, mConfig->core1_out_buff_size);
            mConfig->channels =2;
            mConfig->samplerate = 48000;

            mConfig->init_argc = 0;
            mConfig->init_argv[0] = (char *)malloc(DTSX_PARAM_COUNT_MAX * DTSX_PARAM_STRING_LEN);
            if (mConfig->init_argv[0] == NULL) {
                ALOGE("%s malloc argv memory failed!", __func__);
                return;
            } else {
                memset(mConfig->init_argv[0], 0, DTSX_PARAM_COUNT_MAX * DTSX_PARAM_STRING_LEN);
                for (int i = 1; i < DTSX_PARAM_COUNT_MAX; i++) {
                    mConfig->init_argv[i] = mConfig->init_argv[0] + (DTSX_PARAM_STRING_LEN * i);
                }
            }
	    }
    }
    mEos = false;
    mSetUp = false;
}

c2_status_t C2AudioDTSXDecoder::onInit() {
    LOG_LINE();

    status_t err = initDecoder();

    LOG_LINE();
    return err == OK ? C2_OK : C2_CORRUPTED;
}

c2_status_t C2AudioDTSXDecoder::onStop() {
    LOG_LINE();

    mBuffersInfo.clear();
    {
        AutoMutex l(mFlushLock);
        mRunning = false;
    }
    mAbortPlaying = true;

    return C2_OK;
}

void C2AudioDTSXDecoder::onRelease() {
    LOG_LINE();
    {
        AutoMutex l(mFlushLock);
        mRunning = false;
    }

    mInputFlushing = false;
    mOutputFlushing = false;
    mInputFlushDone = false;
    mOutputFlushDone = false;
    onStop();
    tearDown();
}

void C2AudioDTSXDecoder::onReset() {
    LOG_LINE();
}

c2_status_t C2AudioDTSXDecoder::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    LOG_LINE();

    return C2_OK;
}

c2_status_t C2AudioDTSXDecoder::onFlush_sm() {
    LOG_LINE();
    mBuffersInfo.clear();

    return C2_OK;
}

c2_status_t C2AudioDTSXDecoder::drainEos(
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

void C2AudioDTSXDecoder::drainOutBuffer(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool,
        bool eos) {

    while (!mBuffersInfo.empty()) {
        Info &outInfo = mBuffersInfo.front();
        int numFrames = outInfo.decodedSizes.size();
        int outputDataSize =  mConfig->outputlen;
        if (mConfig->debug_print) {
            ALOGI("%s outputDataSize:%d,  outInfo numFrames:%d,frameIndex = %" PRIu64 "",__func__, outputDataSize, numFrames, outInfo.frameIndex);
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
            /*coverity[use_after_free]*/
            ALOGV("%s  mBuffersInfo is %s, out timestamp %" PRIu64 " / %u", __func__, mBuffersInfo.empty()?"null":"not null", outInfo.timestamp, block ? block->capacity() : 0);
        }
    }
}

void C2AudioDTSXDecoder::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {

    // Initialize output work
    work->result = C2_OK;
    work->workletsProcessed = 1u;
    work->worklets.front()->output.configUpdate.clear();
    work->worklets.front()->output.flags = work->input.flags;
    int ret = 0, bits_per_sample = 0;;

    int prevSampleRate = pcm_out_info.sample_rate;
    int prevNumChannels = pcm_out_info.channel_num;

    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;
    if (mConfig->debug_print) {
        ALOGI("%s input.flags:0x%x  eos:%d", __func__, work->input.flags, eos);
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
        ALOGI("%s() inInfo.bufferSize:%zu, frameIndex:%" PRIu64 ", timestamp:%" PRIu64 "", __func__, inInfo.bufferSize, inInfo.frameIndex, inInfo.timestamp);
    }

    if (inBuffer_nFilledLen) {
        mConfig->outputlen_frames = 0;
        decode_offset += inBuffer_nFilledLen;
    }

    mConfig->pInput = (char *)inBuffer;
    mConfig->inputlen = inBuffer_nFilledLen;
    mConfig->inputlen_used = 0;
    mConfig->pOutput = mOutputBuffer;
    mConfig->outputlen_pcm = 0;
    mConfig->outputlen_raw = 0;
    mConfig->outputlen = 0;
    if (mConfig->debug_dump == 1) {
        dump("/data/vendor/audiohal/c2_dtsx_in.dts", mConfig->pInput, mConfig->inputlen);
    }

    if (mConfig->digital_raw < 3) {
        ret=(_aml_dts_decoder_process)(mConfig->p_dtsx_dec_inst,
                                         (const unsigned char *)mConfig->pInput,
                                         mConfig->inputlen,
                                         mConfig->a_dtsx_pp_output,
                                         (unsigned int *)&mConfig->outputlen_pcm);
        if ((ret != 0) || (mConfig->outputlen_pcm == 0)) {
            ALOGW("[%s:%d] dtsx decode fail:%d, outlen_pcm:%d", __func__, __LINE__, ret, mConfig->outputlen_pcm);
            mConfig->outputlen_pcm = 0;
        } else {
            if (ret == 0) {
                ret = (_aml_dts_metadata_update)(mConfig->p_dtsx_dec_inst, mConfig->p_dtsx_pp_inst);
            } else {
                ALOGW("[%s:%d] dtsx metadata update fail:%d", __func__, __LINE__, ret);
            }
            if (mConfig->outputlen_pcm > mConfig->core1_out_buff_size) {
                ALOGI("[%s:%d] realloc decode buffer from (%d) to (%u)", __func__, __LINE__, mConfig->core1_out_buff_size, mConfig->outputlen_pcm);
                mConfig->core1_out_pcm = (char *)realloc(mConfig->core1_out_pcm, mConfig->outputlen_pcm);
                if (mConfig->core1_out_pcm == NULL) {
                    ALOGE("[%s:%d] realloc for decode buffer(%u) failed", __func__, __LINE__, mConfig->outputlen_pcm);
                    return;
                }
                mConfig->core1_out_buff_size = mConfig->outputlen_pcm;
            }
            memcpy(mConfig->core1_out_pcm, mConfig->a_dtsx_pp_output[DTSX_OUTPUT_SPK], mConfig->outputlen_pcm);

            ret = (_aml_dts_postprocess_proc)(mConfig->p_dtsx_pp_inst,
                                (const unsigned char *)mConfig->core1_out_pcm, mConfig->outputlen_pcm,
                                mConfig->a_dtsx_pp_output, mConfig->a_dtsx_pp_output_size);
            if (ret == 0) {
                if (mConfig->digital_raw == 0 && mConfig->a_dtsx_pp_output_size[DTSX_OUTPUT_SPK] > 0) {
                    _dtsx_pcm_output();
                } else {
                    _dtsx_raw_output();
                }
            } else {
                ALOGE("[%s:%d] dtsx post process fail:%d", __func__, __LINE__, ret);
            }

            ret = (_aml_dts_postprocess_get_out_info)(mConfig->p_dtsx_pp_inst, DTSX_OUTPUT_SPK,
                                                &pcm_out_info.sample_rate,
                                                &pcm_out_info.channel_num,
                                                &bits_per_sample);
            if (ret != 0) {
                ALOGW("[%s:%d] _aml_dts_postprocess_get_out_info fail:%d", __func__, __LINE__, ret);
            }
            if (mConfig->debug_print == 1) {
                ALOGI("pcm_out_info.sample_rate=%d, pcm_out_info.channel_num=%d",pcm_out_info.sample_rate, pcm_out_info.channel_num);
            }
        }
        mConfig->outputlen_frames = mConfig->outputlen / mConfig->channels;//total decoded frames
    } else {
        // This case is designed for Xiaomi. When digital_raw=3,
        // omx will bypass audio es, and the player reads back the audio es,
        // and send to audioflinger in offload mode.
        memcpy((char *)mConfig->pOutput, (char *)mConfig->pInput, mConfig->inputlen);
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

bool C2AudioDTSXDecoder::unload_dtsx_decoder_lib(){
    LOG_LINE();
    _aml_dts_decoder_init = NULL;
    _aml_dts_decoder_process = NULL;
    _aml_dts_decoder_deinit = NULL;
    _aml_dts_decoder_get_output_info = NULL;
    _aml_dts_postprocess_init = NULL;
    _aml_dts_postprocess_deinit = NULL;
    _aml_dts_postprocess_proc = NULL;
    _aml_dts_metadata_update = NULL;
    _aml_dts_postprocess_get_out_info = NULL;
    _aml_dts_postprocess_dynamic_parameter_set = NULL;
    if (gDtsxDecoderLibHandler != NULL) {
        dlclose(gDtsxDecoderLibHandler);
        gDtsxDecoderLibHandler = NULL;
    }

    return true;
}

bool C2AudioDTSXDecoder::load_dtsx_decoder_lib(const char *filename){
     LOG_LINE();
    gDtsxDecoderLibHandler = dlopen(filename, RTLD_NOW);
    //open 32bit so failed, here try to open the 64bit dtsx so.
    if (gDtsxDecoderLibHandler == NULL) {
        gDtsxDecoderLibHandler = dlopen(DTSX_LIB64_PATH_A, RTLD_NOW);
        ALOGI("%s, 64bit lib:%s, gDtsxDecoderLibHandler:%p\n", __FUNCTION__, DTSX_LIB64_PATH_A, gDtsxDecoderLibHandler);
    }
    if (!gDtsxDecoderLibHandler) {
        ALOGE("%s, failed to open (libHwAudio_dtsx.so), %s\n", __FUNCTION__, dlerror());
        return false;
    } else {
        ALOGV("<%s::%d>--[gDtsxDecoderLibHandler]", __FUNCTION__, __LINE__);
    }

    _aml_dts_decoder_init = (int (*)(void **, unsigned int, const char **))dlsym(gDtsxDecoderLibHandler, "dtsx_decoder_init");
    if (_aml_dts_decoder_init == NULL) {
        ALOGE("%s,can't find decoder lib,%s\n", __FUNCTION__, dlerror());
        return false;
    } else {
        ALOGV("<%s::%d>--[dts_decoder_init:]", __FUNCTION__, __LINE__);
    }

    _aml_dts_decoder_process = (int (*)(void *, const unsigned char *, unsigned int, unsigned char **,unsigned int *))dlsym(gDtsxDecoderLibHandler, "dtsx_decoder_process");
    if (_aml_dts_decoder_process == NULL) {
        ALOGE("%s,can't find decoder lib,%s\n", __FUNCTION__, dlerror());
        return false;
    } else {
        ALOGV("<%s::%d>--[dts_decoder_process:]", __FUNCTION__, __LINE__);
    }

    _aml_dts_decoder_deinit = (int (*)(void *))dlsym(gDtsxDecoderLibHandler, "dtsx_decoder_deinit");
    if (_aml_dts_decoder_deinit == NULL) {
        ALOGE("%s,can't find decoder lib,%s\n", __FUNCTION__, dlerror());
        return false;
    } else {
        ALOGV("<%s::%d>--[dts_decoder_deinit:]", __FUNCTION__, __LINE__);
    }

    _aml_dts_decoder_get_output_info = (int (*)(void *, int, int *, int *, int *))dlsym(gDtsxDecoderLibHandler, "dtsx_decoder_get_out_info");
    if (_aml_dts_decoder_get_output_info == NULL) {
        ALOGE("%s,can not find decoder getinfo function,%s\n", __FUNCTION__, dlerror());
        return false;
    } else {
        ALOGV("<%s::%d>--[dts_decoder_getinfo:]", __FUNCTION__, __LINE__);
    }

    _aml_dts_postprocess_init = (int (*)(void **, unsigned int, const char **))dlsym(gDtsxDecoderLibHandler, "dtsx_postprocess_init");
    if (_aml_dts_postprocess_init == NULL) {
        ALOGE("%s,can not find decoder getinfo function,%s\n", __FUNCTION__, dlerror());
        return false;
    } else {
        ALOGV("<%s::%d>--[dts_postprocess_init:]", __FUNCTION__, __LINE__);
    }

    _aml_dts_postprocess_deinit = (int (*)(void *))dlsym(gDtsxDecoderLibHandler, "dtsx_postprocess_deinit");
    if (_aml_dts_postprocess_deinit == NULL) {
        ALOGE("%s,can not find decoder getinfo function,%s\n", __FUNCTION__, dlerror());
        return false;
    } else {
        ALOGV("<%s::%d>--[dts_postprocess_deinit:]", __FUNCTION__, __LINE__);
    }

    _aml_dts_postprocess_proc = (int (*)(void *, const unsigned char *, unsigned int, unsigned char **,unsigned int *))dlsym(gDtsxDecoderLibHandler, "dtsx_postprocess_proc");
    if (_aml_dts_postprocess_proc == NULL) {
        ALOGE("%s,can not find decoder getinfo function,%s\n", __FUNCTION__, dlerror());
        return false;
    } else {
        ALOGV("<%s::%d>--[dts_postprocess_proc:]", __FUNCTION__, __LINE__);
    }

    _aml_dts_metadata_update = (int (*)(void *, void *))dlsym(gDtsxDecoderLibHandler, "dtsx_metadata_update");
    if (_aml_dts_metadata_update == NULL) {
        ALOGE("%s,can not find decoder getinfo function,%s\n", __FUNCTION__, dlerror());
        return false;
    } else {
        ALOGV("<%s::%d>--[dts_metadata_update:]", __FUNCTION__, __LINE__);
    }

    _aml_dts_postprocess_get_out_info = (int (*)(void *, int, int *, int *, int *))dlsym(gDtsxDecoderLibHandler, "dtsx_postprocess_get_out_info");
    if (_aml_dts_postprocess_get_out_info == NULL) {
        ALOGE("%s,can not find postprocess getinfo function,%s\n", __FUNCTION__, dlerror());
    } else {
        ALOGV("<%s::%d>--[dts_postprocess_getinfo:]", __FUNCTION__, __LINE__);
    }

    _aml_dts_postprocess_dynamic_parameter_set = (int (*)(void *, unsigned int, const char **))dlsym(gDtsxDecoderLibHandler, "dtsx_postprocess_dynamic_parameter_set");
    if (_aml_dts_postprocess_dynamic_parameter_set == NULL) {
        ALOGE("%s,can not find postprocess dynamic_parameter_set function,%s\n", __FUNCTION__, dlerror());
    } else {
        ALOGV("<%s::%d>--[dts_postprocess_dynamic_parameter_set:]", __FUNCTION__, __LINE__);
    }

    ALOGI("[%s:%d] out", __func__, __LINE__);
    return true;
}

bool C2AudioDTSXDecoder::setUpAudioDecoder_l() {
    if (load_dtsx_decoder_lib(DTSX_LIB_PATH_A)) {
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
            _aml_dtsx_dualcore_init();
        }
        mIsFirst = true;
        return true;
    }
    mIsFirst = true;
    return false;
}

bool C2AudioDTSXDecoder::tearDownAudioDecoder_l() {
    LOG_LINE();

    if (mConfig) {
        if (mConfig->core1_out_pcm) {
            free(mConfig->core1_out_pcm);
            mConfig->core1_out_pcm = NULL;
        }
        if (mConfig->init_argv[0]) {
            free(mConfig->init_argv[0]);
            mConfig->init_argv[0] = NULL;
        }
    }

    if (mConfig != NULL) {
        free(mConfig);
        mConfig = NULL;
    }
    if (gDtsxDecoderLibHandler != NULL)
        unload_dtsx_decoder_lib();
    return true;

    return true;
}

bool C2AudioDTSXDecoder::isSetUp() {
    AutoMutex l(mSetUpLock);
    return mSetUp;
}

status_t C2AudioDTSXDecoder::initDecoder() {
    status_t status = UNKNOWN_ERROR;

    LOG_LINE();
    AutoMutex l(mSetUpLock);
    if (mSetUp) {
        ALOGW("Trying to set up stream when you already have.");
        return OK;
    }
    if (!setUpAudioDecoder_l()) {
        LOG_LINE();
        ALOGE("setUpDTSXAudioDecoder_l failed.");
        tearDownAudioDecoder_l();
        return C2_OMITTED;
    }
    ALOGI("C2AudioDTSXDecoder setUp done\n");
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

uint32_t C2AudioDTSXDecoder::maskFromCount(uint32_t channelCount) {
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

class C2AudioDTSXDecoderFactory : public C2ComponentFactory {
public:
    C2AudioDTSXDecoderFactory(C2String decoderName) : mDecoderName(decoderName),
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
                new C2AudioDTSXDecoder(mDecoderName.c_str(),
                              id,
                              std::make_shared<C2AudioDTSXDecoder::IntfImpl>(mDecoderName.c_str(), mHelper)));
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id, std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
            //ALOGI("in %s, id:%d,  start to create C2ComponentInterface", __func__, id);
            UNUSED(deleter);
        *interface = std::shared_ptr<C2ComponentInterface>(
                new AudioDecInterface<C2AudioDTSXDecoder::IntfImpl>(
                        mDecoderName.c_str(), id, std::make_shared<C2AudioDTSXDecoder::IntfImpl>(mDecoderName.c_str(), mHelper)));
        return C2_OK;
    }

    virtual ~C2AudioDTSXDecoderFactory() override = default;

private:
    const C2String mDecoderName;
    std::shared_ptr<C2ReflectorHelper> mHelper;
};


}  // namespace android

#define CreateC2AudioDecFactory(type) \
    extern "C" ::C2ComponentFactory* CreateC2AudioDTSXDecoder##type##Factory() { \
         ALOGV("create component %s ", #type); \
         std::string type_dtse = "DTSE"; \
         std::string type_dtshd = "DTSHD"; \
         std::string type_dts = "DTS"; \
         std::string type_dtsuhd = "DTSUHD"; \
         if (!type_dtshd.compare(#type)) { \
            return new ::android::C2AudioDTSXDecoderFactory(COMPONENT_NAME_DTSHD); \
         } else if (!type_dtse.compare(#type)) { \
            return new ::android::C2AudioDTSXDecoderFactory(COMPONENT_NAME_DTSE); \
         } else if (!type_dtsuhd.compare(#type)) { \
            return new ::android::C2AudioDTSXDecoderFactory(COMPONENT_NAME_DTSUHD); \
         } else { \
            return new ::android::C2AudioDTSXDecoderFactory(COMPONENT_NAME_DTS); \
         } \
    }

#define DestroyC2AudioDecFactory(type) \
    extern "C" void DestroyC2AudioDTSXDecoder##type##Factory(::C2ComponentFactory* factory) {\
        delete factory;\
    }

CreateC2AudioDecFactory(DTS)
DestroyC2AudioDecFactory(DTS)
CreateC2AudioDecFactory(DTSE)
DestroyC2AudioDecFactory(DTSE)
CreateC2AudioDecFactory(DTSHD)
DestroyC2AudioDecFactory(DTSHD)
CreateC2AudioDecFactory(DTSUHD)
DestroyC2AudioDecFactory(DTSUHD)