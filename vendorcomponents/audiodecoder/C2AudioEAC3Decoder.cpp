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
#define LOG_TAG "Amlogic_C2AudioEAC3Decoder"
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
#include "C2AudioEAC3Decoder.h"
#include "aml_ac3_decoder_api.h"
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


#define DOLBY_MS12_V24_LIB_PATH   "/dev/audio_utils"
#define DOLBY_MS12_V24_LIB_PATH_A "/odm/lib/ms12/libdolbyms12.so" //MS12 v2.4
#define DOLBY_MS12_V24_LIB_PATH_B "/vendor/lib/ms12/libdolbyms12.so" //MS12 v2.4
#define DOLBY_MS12_V1_LIB_PATH_A "/odm/lib/libdolbyms12.so" //MS12 v1
#define DOLBY_MS12_V1_LIB_PATH_B "/vendor/lib/libdolbyms12.so" //MS12 v1

constexpr char COMPONENT_NAME_AC3[] = "c2.amlogic.audio.decoder.ac3";
constexpr char COMPONENT_NAME_EAC3[] = "c2.amlogic.audio.decoder.eac3";

static bool component_is_eac3 = 0;

namespace android {

class C2AudioEAC3Decoder::IntfImpl : public AudioDecInterface<void>::BaseParams {
public:
    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper)
        : AudioDecInterface<void>::BaseParams(
                helper,
                component_is_eac3 ? COMPONENT_NAME_EAC3 : COMPONENT_NAME_AC3,
                C2Component::KIND_DECODER,
                C2Component::DOMAIN_AUDIO,
                component_is_eac3 ? MEDIA_MIMETYPE_AUDIO_EAC3 : MEDIA_MIMETYPE_AUDIO_AC3) {

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
                .withConstValue(new C2StreamMaxBufferSizeInfo::input(0u, 8192))
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

/*static unsigned long amsysfs_get_sysfs_ulong(const char *path)
{
    int fd;
    char bcmd[24]="";
    unsigned long num=0;
    if ((fd = open(path, O_RDONLY)) >=0) {
        read(fd, bcmd, sizeof(bcmd));
        num = strtoul(bcmd, NULL, 0);
        close(fd);
    } else {
        ALOGI("unable to open file \n");
    }
    return num;
}*/

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
    }else
        ALOGE("error: open file failed\n");
}
const uint16_t aml_ac3_frame_size_tab[38][3] = {
    { 64,   69,   96   },
    { 64,   70,   96   },
    { 80,   87,   120  },
    { 80,   88,   120  },
    { 96,   104,  144  },
    { 96,   105,  144  },
    { 112,  121,  168  },
    { 112,  122,  168  },
    { 128,  139,  192  },
    { 128,  140,  192  },
    { 160,  174,  240  },
    { 160,  175,  240  },
    { 192,  208,  288  },
    { 192,  209,  288  },
    { 224,  243,  336  },
    { 224,  244,  336  },
    { 256,  278,  384  },
    { 256,  279,  384  },
    { 320,  348,  480  },
    { 320,  349,  480  },
    { 384,  417,  576  },
    { 384,  418,  576  },
    { 448,  487,  672  },
    { 448,  488,  672  },
    { 512,  557,  768  },
    { 512,  558,  768  },
    { 640,  696,  960  },
    { 640,  697,  960  },
    { 768,  835,  1152 },
    { 768,  836,  1152 },
    { 896,  975,  1344 },
    { 896,  976,  1344 },
    { 1024, 1114, 1536 },
    { 1024, 1115, 1536 },
    { 1152, 1253, 1728 },
    { 1152, 1254, 1728 },
    { 1280, 1393, 1920 },
    { 1280, 1394, 1920 },
};

/*const uint8_t ff_ac3_channels_tab[8] = {
    2, 1, 2, 3, 3, 4, 4, 5
};*/

int check_ac3_syncword(const unsigned char *ptr, int size)
{
    if (size < 2)
        return 0;
    if (ptr[0] == 0x0b && ptr[1] == 0x77)
        return 1;
    if (ptr[0] == 0x77 && ptr[1] == 0x0b)
        return 2;

    return 0;
}

int parse_frame_header
    (const unsigned char *frameBuf
    , int length
    ,int *frame_size
    ,int *offset, int *IsEc3){
    int acmod = 0;
    int lfeOn = 0;
    int nIsEc3 = 0;
    int frame_size_code = 0;
    int sr_code = 0;
    int substreamid = 0;
    int numblk_per_frame;
    char inheader[12] = {0};
    int header = 0;
    int i = 0;

    for (i = 0; i < length; ++i) {
        if ((header = check_ac3_syncword(&frameBuf[i], length - i)) > 0) {
            *offset = i;
            break;
        }
    }
    //2 step 1, frame header 0x0b77/0x770b
    if (header == 0) {
        ALOGE("locate frame header 0x0b77/0x770b failed\n");
        goto error;//no frame header, maybe need more data
    }

    //2 step 2, copy 12bytes to inheader,  find one frame
    if (length - *offset < 12) {
        //find the sync word 0x0b77/0x770b,
        //but we need 12bytes which will copy to inheader[12], need more data
        ALOGE("data less than one frame!!!\n");
        goto error;
    }
    else {
        memcpy(inheader, (char *)(frameBuf + *offset), 12);
    }

    if (header == 2) {
        int16_t *p_data = (int16_t *)inheader;
        unsigned int idx;
        unsigned int inheader_len = 12;
        unsigned int top = inheader_len / 2;
        for (idx = 0; idx < top; idx++)
        {
            p_data[idx] = (int16_t) BYTE_REV(p_data[idx]);
        }
    }

    if (length < 12) {
        ALOGE("%s::%d-[len:%d]\n",__FUNCTION__, __LINE__, length);
        goto error;
    }
    else {
        //ALOGV("dolby head:0x%x 0x%x 0x%x 0x%x 0x%x 0x%x \n",
        // inheader[0],inheader[1],inheader[2], inheader[3],inheader[4],inheader[5]);
        int bsid = (inheader[5] >> 3) & 0x1f;//bitstream_id,bit[40,44]
        if (bsid > 16)
            goto error;//invalid bitstream_id
        if (bsid <= 8)
            nIsEc3 = 0;
        else if((bsid <= 16) && (bsid > 10))
            nIsEc3 = 1;
        *IsEc3 = nIsEc3;

        if (nIsEc3 == 0) {
            //ALOGI("%02x",inheader[6]);
            int use_bits = 0;

            substreamid = 0;
            sr_code = inheader[4]>>6;
            if (sr_code == 3) {
                ALOGE("%s::%d-[error *sr_code:%d]", __FUNCTION__, __LINE__, sr_code);
                goto error;
            }
            frame_size_code = inheader[4]&0x3F;
            if (frame_size_code > 37) {
                ALOGE("%s::%d-[error frame_size_code:%d]", __FUNCTION__, __LINE__, frame_size_code);
                goto error;
            }
            acmod = (inheader[6] >> 5) & 0x7;// 3bits
            use_bits = use_bits+3;
            lfeOn = (inheader[6] >>(8 - use_bits -1))&0x1; // 1bit
            *frame_size = aml_ac3_frame_size_tab[frame_size_code][sr_code] * 2;
            numblk_per_frame = 6;
        }
        else {
            int numblkscod = 0;
            //int strmtype = (inheader[2] >> 6) & 0x3;
            //int substreamid = (inheader[2]>>3)&0x7;
            *frame_size = ((inheader[2]&0x7)*0x100 + inheader[3] + 1) << 1;
            sr_code = inheader[4]>>6;
            acmod = (inheader[4] >> 1) & 0x7;
            lfeOn = inheader[4] &0x1;
            numblkscod = (sr_code == 0x3) ? 0x3 : ( (inheader[4] >> 4) & 0x3);
            numblk_per_frame = (numblkscod == 0x3) ? 6 : (numblkscod + 1);
        }
    }
    return 0;
error:
    return 1;
}

C2AudioEAC3Decoder::C2AudioEAC3Decoder(
        const char *name,
        c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl)
    : C2AudioDecComponent(std::make_shared<AudioDecInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl),
    nAudioCodec(1),
    digital_raw(0),
    nIsEc3(1),
    adec_call(false),
    mAnchorTimeUs(0),
    decoder_offset(0),
    mNumFramesOutput(0)
{
    ALOGV("%s() %d  name:%s", __func__, __LINE__, name);
    {
        AutoMutex l(mSetUpLock);
        initializeState_l();
    }

    gDDPDecoderLibHandler = NULL;
    handle = NULL;
    memset(&pcm_out_info, 0, sizeof(pcm_out_info));
    spdif_addr = (char *)malloc(6144*4*3);
    memset(spdif_addr, 0, 6144*4*3);
    mRemainBufLen = 6144 * 4;
    mRemainBuffer = (unsigned char *)malloc(mRemainBufLen);
    mRemainLen = 0;
    memset(sysfs_buf, 0, sizeof(sysfs_buf));
    mOutBuffer = (int16_t *)malloc(6144*4);//4*32ms size
}

C2AudioEAC3Decoder::~C2AudioEAC3Decoder() {
    ALOGV("%s() %d", __func__, __LINE__);
    onRelease();

    if (mOutBuffer != NULL) {
        free(mOutBuffer);
        mOutBuffer = NULL;
    }
    if (mConfig != NULL) {
        free(mConfig);
        mConfig = NULL;
    }
    if (spdif_addr != NULL) {
        free(spdif_addr);
        spdif_addr = NULL;
    }
    if (mRemainBuffer != NULL) {
        free(mRemainBuffer);
        mRemainBuffer = NULL;
        mRemainLen= 0;
        mRemainBufLen = 0;
    }
    mDecodingErrors = 0;
    mTotalDecodedFrames = 0;
    /*char value[PROPERTY_VALUE_MAX];
    if (property_get(AML_DEBUG_AUDIOINFO_REPORT_PROPERTY, value, NULL)) {
        sprintf(sysfs_buf, "decoded_err %d", mDecodingErrors);
        amsysfs_set_sysfs_str(REPORT_DECODED_INFO, sysfs_buf);
        sprintf(sysfs_buf, "decoded_frames %d", mTotalDecodedFrames);
        amsysfs_set_sysfs_str(REPORT_DECODED_INFO, sysfs_buf);
    }
    memset(sysfs_buf, 0, sizeof(sysfs_buf));*/

    LOGV("%s() %d  exit", __func__, __LINE__);
}

void C2AudioEAC3Decoder::initializeState_l() {
    LOG_LINE();
    {
        AutoMutex l(mConfigLock);
        mConfig = (AC3DecoderExternal *)malloc(sizeof(AC3DecoderExternal));
        memset(mConfig, 0, sizeof(AC3DecoderExternal));
        mConfig->num_channels =2;
        mConfig->samplingRate = 48000;
    }
    mEos = false;
    mSetUp = false;
}


bool C2AudioEAC3Decoder::tearDownAudioDecoder_l() {
    LOG_LINE();
    if (mConfig != NULL) {
        free(mConfig);
        mConfig = NULL;
    }
    if (spdif_addr != NULL) {
        free(spdif_addr);
        spdif_addr = NULL;
    }
    if (gDDPDecoderLibHandler != NULL)
        unload_ddp_decoder_lib();

    return true;
}

bool C2AudioEAC3Decoder::unload_ddp_decoder_lib(){
    LOG_LINE();
    if (ddp_decoder_cleanup != NULL && handle != NULL) {
        (*ddp_decoder_cleanup)(handle);
        handle = NULL;
    }
    ddp_decoder_init = NULL;
    ddp_decoder_process = NULL;
    ddp_decoder_cleanup = NULL;
    if (gDDPDecoderLibHandler != NULL) {
        dlclose(gDDPDecoderLibHandler);
        gDDPDecoderLibHandler = NULL;
    }
    return true;
}

bool C2AudioEAC3Decoder::load_license_decoder_lib(const char *filename){

    gDDPDecoderLibHandler = dlopen(filename, RTLD_NOW);

    if (!gDDPDecoderLibHandler) {
        ALOGE("%s, failed to open filename %s dlerror() %s\n", __FUNCTION__, filename, dlerror());
        goto Error;
    } else {
        ALOGV("<%s::%d>--[gDDPDecoderLibHandler:%p]", __FUNCTION__, __LINE__, gDDPDecoderLibHandler);
    }

    ddp_decoder_init = (int (*)(int, int,void **))dlsym(gDDPDecoderLibHandler, "ddp_decoder_init");
    if (ddp_decoder_init == NULL) {
        ALOGE("%s,cant find decoder lib,%s\n", __FUNCTION__, dlerror());
        goto Error;
    } else {
        ALOGV("<%s::%d>--[ddp_decoder_init:]", __FUNCTION__, __LINE__);
    }

    ddp_decoder_process = (int (*)(char * ,int ,int *,int ,char *,int *,struct pcm_info *,char *,int *,void *))dlsym(gDDPDecoderLibHandler, "ddp_decoder_process");
    if (ddp_decoder_process == NULL) {
        ALOGE("%s,cant find decoder lib,%s\n", __FUNCTION__, dlerror());
        goto Error;
    } else {
        ALOGV("<%s::%d>--[ddp_decoder_process:]", __FUNCTION__, __LINE__);
    }

    ddp_decoder_cleanup = (int (*)(void *))dlsym(gDDPDecoderLibHandler, "ddp_decoder_cleanup");
    if (ddp_decoder_cleanup == NULL) {
        ALOGE("%s,cant find decoder lib,%s\n", __FUNCTION__, dlerror());
        goto Error;
    } else {
        ALOGV("<%s::%d>--[ddp_decoder_cleanup:]", __FUNCTION__, __LINE__);
    }
    return true;
Error:
    unload_ddp_decoder_lib();
    return false;
}

bool C2AudioEAC3Decoder::load_ddp_decoder_lib(){
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
    if (!ret && load_license_decoder_lib(DOLBY_MS12_V1_LIB_PATH_A)) {
        ret = true;
    }
    if (!ret && load_license_decoder_lib(DOLBY_MS12_V1_LIB_PATH_B)) {
        ret = true;
    }
    if (!ret) {
        ret = load_license_decoder_lib("libHwAudio_dcvdec.so");
    }

    return ret;
}

bool C2AudioEAC3Decoder::setUpAudioDecoder_l() {

    if (load_ddp_decoder_lib()) {

        char value[PROPERTY_VALUE_MAX];
        //config debug info
        mConfig->debug_print = 0;
        mConfig->debug_dump = 0;
        memset(value,0,sizeof(value));

        if ((property_get("vendor.media.c2.audio.debug",value,NULL) > 0) &&
            (!strcmp(value,"1")||!strcmp(value,"true"))) {
            mConfig->debug_print = 1;
        }
        memset(value,0,sizeof(value));

        if ((property_get("vendor.media.c2.audio.dump",value,NULL) > 0) &&
            (!strcmp(value,"1")||!strcmp(value,"true"))) {
            mConfig->debug_dump = 1;
        }

        digital_raw = mIntf->getPassthroughEnable();
        if (digital_raw >= 3) {
            if (digital_raw == 3) {
                adec_call = false;
            } else {
                digital_raw = digital_raw  - 3;
                adec_call = true;
            }
            ALOGI("adec_call % d digital_raw %d ", adec_call, digital_raw);
        }

        /* nAudioCodec: 1 - AC3,  2 - EAC3 */
        nAudioCodec = component_is_eac3 ? 2 : 1;

        ALOGV("nAudioCodec %d, digital_raw %d adec_call %d \n", nAudioCodec, digital_raw, adec_call);
        if (digital_raw <= 1 ) {/*ac3,eac3 spdif mode passthrough*/
            (*ddp_decoder_init)(1, 1,&handle);
        } else if (digital_raw == 2 && (nAudioCodec == 1)) {/*ac3 hdmi mode passthrough*/
            (*ddp_decoder_init)(1, 1,&handle);
        } else if (digital_raw == 2 && (nAudioCodec == 2)) {/*eac3 hdmi mode passthrough*/
            (*ddp_decoder_init)(1, 2,&handle);
        } else if (digital_raw == 3) {
            // do nothing...
            // This case is designed for Xiaomi. When digital_raw=3,
            // omx will bypass audio es, and the player reads back the audio es,
            // and send to audioflinger in offload mode.
        }

        mIsFirst = true;
       return true;
    }
    mIsFirst = true;
    return false;
}

bool C2AudioEAC3Decoder::setUp() {
    LOG_LINE();
    AutoMutex l(mSetUpLock);
    if (mSetUp) {
        LOGW("Trying to set up stream when you already have.");
        return false;
    }

    if (!setUpAudioDecoder_l()) {
        LOGE("setUpAC3AudioDecoder_l failed.");
        tearDownAudioDecoder_l();
        return false;
    }
    ALOGI("C2AudioEAC3Decoder setUp done\n");
    mSetUp = true;
    return true;
}

bool C2AudioEAC3Decoder::tearDown() {
    LOG_LINE();
    AutoMutex l(mSetUpLock);
    if (mSetUp) {
        tearDownAudioDecoder_l();
    }
    return true;
}

bool C2AudioEAC3Decoder::isSetUp() {
    AutoMutex l(mSetUpLock);
    return mSetUp;
}

c2_status_t C2AudioEAC3Decoder::onInit() {
    ALOGV("%s() %d", __func__, __LINE__);

    status_t err = initDecoder();

    ALOGV("%s() %d exit", __func__, __LINE__);
    return err == OK ? C2_OK : C2_CORRUPTED;
}

c2_status_t C2AudioEAC3Decoder::onStop() {
    ALOGV("%s() %d", __func__, __LINE__);
    mAbortPlaying = true;

    return C2_OK;
}

void C2AudioEAC3Decoder::onReset() {
    ALOGV("%s() %d", __func__, __LINE__);
    (void)onStop();

    if (handle == NULL) {
        return;
    }

    if (ddp_decoder_cleanup != NULL && handle != NULL) {
        (*ddp_decoder_cleanup)(handle);
        handle = NULL;
    }

    if (digital_raw == 1 ) {/*ac3,eac3 spdif mode passthrough*/
        (*ddp_decoder_init)(1, 1,&handle);
    } else if (digital_raw == 2 && (nAudioCodec == 1)) {/*ac3 hdmi mode passthrough*/
        (*ddp_decoder_init)(1, 1,&handle);
    } else if (digital_raw == 2 && (nAudioCodec == 2)) {/*eac3 hdmi mode passthrough*/
        (*ddp_decoder_init)(1, 2,&handle);
    } else {
        (*ddp_decoder_init)(1, 1,&handle);
    }

    memset (mRemainBuffer, 0 ,mRemainLen);
    mRemainLen = 0;
}

void C2AudioEAC3Decoder::onRelease() {
    LOG_LINE();
    if (ddp_decoder_cleanup != NULL && handle != NULL) {
        (*ddp_decoder_cleanup)(handle);
        handle = NULL;
    }
    onStop();

    tearDown();
    mSetUp = false;
}

status_t C2AudioEAC3Decoder::initDecoder() {
    ALOGV("initDecoder()");
    status_t status = UNKNOWN_ERROR;

    if (setUp()) {
    }

    if (!isSetUp()) {
        LOG_LINE();
        return C2_OMITTED;
    }
    mAbortPlaying = false;

    //setConfig default
    nAudioCodec = 1;
    nIsEc3 = 0;
    /*ac3 eac3 default decoder output channel is 2, it depends on decoder lib */
    mConfig->num_channels = 2;
    mConfig->samplingRate = 48000;


    status = OK;
    return status;
}

void C2AudioEAC3Decoder::drainOutBuffer(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool,
        bool eos) {

    while (!mBuffersInfo.empty()) {
        Info &outInfo = mBuffersInfo.front();
        int numFrames = outInfo.decodedSizes.size();
        int outputDataSize =  mConfig->outputFrameSize;
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
                size_t bufferSize = mConfig->outputFrameSize;
                c2_status_t err = pool->fetchLinearBlock(bufferSize, usage, &block);
                if (err != C2_OK) {
                    ALOGE("failed to fetch a linear block (%d)", err);
                    return std::bind(fillEmptyWork, _1, C2_NO_MEMORY);
                }
                C2WriteView wView = block->map().get();
                int16_t *outBuffer = reinterpret_cast<int16_t *>(wView.data());
                memcpy(outBuffer, mConfig->pOutputBuffer, mConfig->outputFrameSize);
                if (mConfig->debug_dump == 1) {
                    dump("/data/vendor/audiohal/c2_audio_out_2.pcm", (char *)outBuffer, mConfig->outputFrameSize);
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

void C2AudioEAC3Decoder::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    int spdif_len = 0;
    int ret = 0;

    // Initialize output work
    work->result = C2_OK;
    work->workletsProcessed = 1u;
    work->worklets.front()->output.configUpdate.clear();
    work->worklets.front()->output.flags = work->input.flags;

    int prevSampleRate = pcm_out_info.sample_rate;
    int prevNumChannels = pcm_out_info.channel_num;
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

    Info inInfo;
    inInfo.frameIndex = work->input.ordinal.frameIndex.peeku();
    inInfo.timestamp = work->input.ordinal.timestamp.peeku();
    inInfo.bufferSize = inBuffer_nFilledLen;
    inInfo.decodedSizes.clear();
    if (mConfig->debug_print) {
        ALOGV("%s() inInfo.bufferSize:%zu, frameIndex:%" PRIu64 ", timestamp:%" PRIu64 "", __func__, inInfo.bufferSize, inInfo.frameIndex, inInfo.timestamp);
    }


    uint32_t inBuffer_offset = 0;
    //uint32_t out_offset = 0;
    uint64_t out_timestamp = 0;
    mConfig->pOutputBuffer = mOutBuffer;
    mConfig->outputFrameSize = 0;
    mConfig->pInputBuffer = inBuffer;
    mConfig->CurrentFrameLength = inBuffer_nFilledLen;

    if (inBuffer_offset == 0 && inBuffer_nFilledLen) {
        mAnchorTimeUs = inInfo.timestamp;
        mNumFramesOutput = 0;
        decoder_offset += inBuffer_nFilledLen;
        if (mConfig->debug_print == 1)
            ALOGI("inHeader->nFilledLen:%zu  inHeader->nTimeStamp %" PRIu64 "",inBuffer_nFilledLen,inInfo.timestamp);

        if (mConfig->debug_dump == 1)
            dump("/data/vendor/audiohal/c2_decoder_in.ac3", (char *)mConfig->pInputBuffer, inBuffer_nFilledLen);
    }

    if (digital_raw < 3) {
        while (inBuffer_nFilledLen > 0) {
            int used_size = 0,framesize = 0, offset = 0;
            bool use_remainbuffer = false;
            if (mRemainLen) {
                int avail_size = mRemainBufLen - mRemainLen;
                int copy_size  = inBuffer_nFilledLen > ((uint32_t) avail_size) ? avail_size : inBuffer_nFilledLen;

                memcpy(mRemainBuffer + mRemainLen, mConfig->pInputBuffer, copy_size);
                ret = parse_frame_header(mRemainBuffer, mRemainLen + copy_size, &framesize, &offset, &nIsEc3);
                //ALOGV("%s() 111 ret:%d, inBuffer_len:%zu  avail_size:%d, framesize:%d  offset:%d, nIsEc3:%d", __func__, ret, inBuffer_nFilledLen, avail_size, framesize, offset,  nIsEc3);
                if (ret == 0 && (framesize - mRemainLen >= 0)) {
                    inBuffer_offset += (framesize - mRemainLen);
                    inBuffer_nFilledLen -= (framesize - mRemainLen);
                    use_remainbuffer = true;
                } else {
                    memset(mRemainBuffer, 0 ,mRemainBufLen);
                    mRemainLen = 0;
                    inBuffer_offset += copy_size;
                    inBuffer_nFilledLen -= copy_size;
                    continue;
                }
            } else {
                mConfig->pInputBuffer = inBuffer + inBuffer_offset;
                mConfig->inputBufferCurrentLength = inBuffer_nFilledLen;
                ret = parse_frame_header(mConfig->pInputBuffer,mConfig->inputBufferCurrentLength,&framesize,&offset,&nIsEc3);
                //ALOGV("%s() 222 ret:%d, inBuffer_len:%zu, framesize:%d  offset:%d, nIsEc3:%d", __func__, ret, inBuffer_nFilledLen, framesize, offset,  nIsEc3);
                if (ret == 0) {
                    if (framesize > mConfig->inputBufferCurrentLength - offset) {
                        mRemainLen = mConfig->inputBufferCurrentLength - offset;
                        inBuffer_nFilledLen  = 0;
                        memcpy(mRemainBuffer,mConfig->pInputBuffer + offset, mRemainLen);
                        framesize = 0;
                    } else {
                        mConfig->pInputBuffer = mConfig->pInputBuffer + offset;
                        mConfig->inputBufferCurrentLength = mConfig->inputBufferCurrentLength - offset;
                        inBuffer_nFilledLen = inBuffer_nFilledLen - offset;
                        inBuffer_offset = inBuffer_offset + offset;
                    }

                } else {
                    mRemainLen = inBuffer_nFilledLen;
                    if (mRemainLen > mRemainBufLen/2)
                        mRemainLen = mRemainBufLen/2;
                    memcpy(mRemainBuffer,mConfig->pInputBuffer + inBuffer_nFilledLen - mRemainLen, mRemainLen);
                    inBuffer_nFilledLen = 0;
                    framesize = 0;
                }
            }
            if (framesize) {
                uint8 *pInputBuffer = use_remainbuffer ? mRemainBuffer : mConfig->pInputBuffer;

                ret = (*ddp_decoder_process)((char *)pInputBuffer
                                                    ,framesize
                                                    ,&used_size
                                                    ,nIsEc3
                                                    ,(char *)mConfig->pOutputBuffer
                                                    ,&mConfig->outputFrameSize
                                                    ,(struct pcm_info *)&pcm_out_info
                                                    ,(char *)spdif_addr
                                                    ,(int *)&spdif_len,
                                                    handle);
                if (mConfig->debug_print == 1)
                    ALOGI("%s ret:%d used_size:%d inHeader->nFilledLen:%zu mRemainLen %d mConfig->outputFrameSize %d nIsEc3:%d",
                        __func__, ret,used_size,inBuffer_nFilledLen,mRemainLen, mConfig->outputFrameSize, nIsEc3);
                if (inBuffer_nFilledLen + mRemainLen >= (uint32_t)used_size) {
                    if (mRemainLen && inBuffer_nFilledLen) {
                        mRemainLen = 0;
                        memset (mRemainBuffer , 0 ,mRemainBufLen);
                    } else {
                        inBuffer_offset += used_size;
                        inBuffer_nFilledLen -= used_size;
                    }
                } else {
                    mDecodingErrors++;
                    char value[PROPERTY_VALUE_MAX];
                    if (property_get(AML_DEBUG_AUDIOINFO_REPORT_PROPERTY, value, NULL)) {
                        int isReportInfo = strtol(value, NULL, 0) & DUMP_AUDIO_INFO_DECODE;
                        if (isReportInfo) {
                            sprintf(sysfs_buf, "decoded_err %d", mDecodingErrors);
                            amsysfs_set_sysfs_str(REPORT_DECODED_INFO, sysfs_buf);
                        } else {
                            sprintf(sysfs_buf, "decoded_err %d", 0);
                            amsysfs_set_sysfs_str(REPORT_DECODED_INFO, sysfs_buf);
                        }
                    }

                    if (mRemainLen && inBuffer_nFilledLen) {
                        memset (mRemainBuffer , 0 ,mRemainBufLen);
                        mRemainLen = 0;
                    } else {
                        //inBuffer_offset += inBuffer_nFilledLen;
                        //inBuffer_nFilledLen = 0;
                    }
                    inBuffer_offset += inBuffer_nFilledLen;
                    inBuffer_nFilledLen = 0;
                    break;//exit for decoder error
                }
                mConfig->num_channels = pcm_out_info.channel_num;
                mConfig->samplingRate = pcm_out_info.sample_rate;
                if (mConfig->outputFrameSize > 0) {
                    inInfo.decodedSizes.push_back(mConfig->outputFrameSize);
                    break;//normally go out.
                }
            }
        }

        if (digital_raw > 0) {
            memset((char *)mConfig->pOutputBuffer, 0, spdif_len);
            memcpy((char *)mConfig->pOutputBuffer, (char *)spdif_addr, spdif_len);
            mConfig->outputFrameSize = spdif_len;
        }

        if (mConfig->num_channels) {
            if (digital_raw > 0 && nIsEc3)
                mNumFramesOutput += (mConfig->outputFrameSize / (mConfig->num_channels * 2 * 4));//total decoded frames
            else
                mNumFramesOutput += (mConfig->outputFrameSize / (mConfig->num_channels * 2));//total decoded frames
        }

        if (mNumFramesOutput > 0) {
            mTotalDecodedFrames += mNumFramesOutput;

            char value[PROPERTY_VALUE_MAX];
            if (property_get(AML_DEBUG_AUDIOINFO_REPORT_PROPERTY, value, NULL)) {
                int isReportInfo = strtol(value, NULL, 0) & DUMP_AUDIO_INFO_DECODE;
                if (isReportInfo) {
                    sprintf(sysfs_buf, "decoded_frames %d", mTotalDecodedFrames / (uint32_t)mNumFramesOutput);
                    amsysfs_set_sysfs_str(REPORT_DECODED_INFO, sysfs_buf);
                } else {
                    sprintf(sysfs_buf, "decoded_frames %d", 0);
                    amsysfs_set_sysfs_str(REPORT_DECODED_INFO, sysfs_buf);
                }
            }
        }

        if (adec_call) {
            out_timestamp = decoder_offset;
        } else {
            if ((mConfig->num_channels) && (mConfig->samplingRate)) {
                out_timestamp = mAnchorTimeUs + (1000000ll * mNumFramesOutput) /  mConfig->samplingRate;
            } else {
                out_timestamp = mAnchorTimeUs;
            }
        }

        mBuffersInfo.push_back(std::move(inInfo));
    } else {
           // This case is designed for Xiaomi. When digital_raw=3,
           // omx will bypass audio es, and the player reads back the audio es,
           // and send to audioflinger in offload mode.
           /* TV-42557:
            * Fill zero data behind the buffer returned by omx.
            * Make the returned buffer size equal to 6144(IEC61937 AC3) bytes.
            * Then the player and audioflinger will think there is enough pcm data.
            */
           int spdif_out_len = 6144; // AC3 IEC61937 Framesize
           memset((char *)mConfig->pOutputBuffer, 0, spdif_out_len);
           memcpy((char *)mConfig->pOutputBuffer, (char *)mConfig->pInputBuffer, mConfig->inputBufferCurrentLength);
           mConfig->outputFrameSize = spdif_out_len;
           inBuffer_offset = inBuffer_nFilledLen;
           inBuffer_nFilledLen = 0;
    }

    //update out config.
    if (!pcm_out_info.sample_rate || !pcm_out_info.channel_num) {
        ALOGW("%s Invalid dolby frame", __func__);
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

c2_status_t C2AudioEAC3Decoder::drainEos(
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

c2_status_t C2AudioEAC3Decoder::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    LOG_LINE();

    return C2_OK;
}

c2_status_t C2AudioEAC3Decoder::onFlush_sm() {
    LOG_LINE();
    mBuffersInfo.clear();

    return C2_OK;
}

void C2AudioEAC3Decoder::drainDecoder() {
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

uint32_t C2AudioEAC3Decoder::maskFromCount(uint32_t channelCount) {
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


class C2AudioEAC3DecoderFactory : public C2ComponentFactory {
public:
    C2AudioEAC3DecoderFactory(C2String decoderName) : mDecoderName(decoderName),
        mHelper(std::static_pointer_cast<C2ReflectorHelper>(
            GetCodec2VendorComponentStore()->getParamReflector())) {
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
            UNUSED(deleter);
            ALOGI("in %s, mDecoderName:%s component_is_eac3:%d", __func__, mDecoderName.c_str(), component_is_eac3);
        *component = std::shared_ptr<C2Component>(
                new C2AudioEAC3Decoder(mDecoderName.c_str(),
                              id,
                              std::make_shared<C2AudioEAC3Decoder::IntfImpl>(mHelper)));
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id, std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
            //ALOGI("in %s, id:%d,  start to create C2ComponentInterface", __func__, id);
            UNUSED(deleter);
        *interface = std::shared_ptr<C2ComponentInterface>(
                new AudioDecInterface<C2AudioEAC3Decoder::IntfImpl>(
                        mDecoderName.c_str(), id, std::make_shared<C2AudioEAC3Decoder::IntfImpl>(mHelper)));
        return C2_OK;
    }

    virtual ~C2AudioEAC3DecoderFactory() override = default;

private:
    const C2String mDecoderName;
    std::shared_ptr<C2ReflectorHelper> mHelper;
};

}  // namespace android


#define CreateC2AudioDecFactory(type) \
    extern "C" ::C2ComponentFactory* CreateC2AudioDecoder##type##Factory() {\
         ALOGV("create component %s ", #type);\
         std::string is_eac3 = "EAC3";\
         if (!is_eac3.compare(#type)) {\
             component_is_eac3 = true;\
             return new ::android::C2AudioEAC3DecoderFactory(COMPONENT_NAME_EAC3);\
         } else {\
            component_is_eac3 = false;\
            return new ::android::C2AudioEAC3DecoderFactory(COMPONENT_NAME_AC3);\
         }\
    }

#define DestroyC2AudioDecFactory(type) \
    extern "C" void DestroyC2AudioDecoder##type##Factory(::C2ComponentFactory* factory) {\
        delete factory;\
    }


CreateC2AudioDecFactory(EAC3)
DestroyC2AudioDecFactory(EAC3)
CreateC2AudioDecFactory(AC3)
DestroyC2AudioDecFactory(AC3)

