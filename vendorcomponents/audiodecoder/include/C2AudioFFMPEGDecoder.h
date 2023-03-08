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

#ifndef ANDROID_C2_AUDIO_FFMPEG_DECODER_H_
#define ANDROID_C2_AUDIO_FFMPEG_DECODER_H_

#include <C2AudioDecComponent.h>
#include "AmlAudioCommon.h"

class AmAudioCodec;

#define BUFFER_MAX_LENGTH 128
#define AML_DEBUG_AUDIOINFO_REPORT_PROPERTY    "vendor.media.audio.info.report.debug"
#define REPORT_DECODED_INFO  "/sys/class/amaudio/codec_report_info"
#define DUMP_AUDIO_INFO_DECODE (0x1000)  //use to enable the audio report info prop

namespace android {

typedef enum DECODER_STATETYPE
{
    DECODER_StateInvalid = -1,
    DECODER_StatePrepare,
    DECODER_StateStart,
    DECODER_StatePause,
    DECODER_StateFlush,
    DECODER_StateWrite,
    DECODER_StateStop,

    DECODER_StateMax = 0xFF,
}DECODER_STATETYPE;


struct C2AudioFFMPEGDecoder : public C2AudioDecComponent {
    class IntfImpl;

    C2AudioFFMPEGDecoder(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2AudioFFMPEGDecoder();

    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(
            const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(
            uint32_t drainMode,
            const std::shared_ptr<C2BlockPool> &pool) override;

private:
    enum {
        kNumDelayBlocksMax      = 8,
    };

    std::shared_ptr<IntfImpl> mIntf;

    struct Info {
        uint64_t frameIndex;
        size_t bufferSize;
        uint64_t timestamp;
        std::vector<int32_t> decodedSizes;
    };
    std::list<Info> mBuffersInfo;

    void initPorts();
    status_t initDecoder();
    bool isConfigured() const;
    void drainDecoder();
    void drainOutBuffer(
            const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2BlockPool> &pool,
            bool eos);
    c2_status_t drainEos(
            uint32_t drainMode,
            const std::shared_ptr<C2BlockPool> &pool,
            const std::unique_ptr<C2Work> &work);
    uint32_t maskFromCount(uint32_t channelCount);

    typedef struct AUDIO_INFO{
        int32_t channels;
        int32_t bitrate;
        int32_t samplerate;
        int32_t bitspersample;
        int32_t blockalign;
        int32_t codec_id;
        int32_t extradata_size;
        uint8_t extradata[16384];
    } AUDIO_INFO_T;
    struct AUDIO_INFO * mAInfo;
    const char * mComponentName;
    const char * mMimeType;
    struct pcm_info{
        int sample_rate;
        int channel_num;
        int bytes_per_sample;
        int bitstream_type;
    };
    struct pcm_info pcm_out_info;

    int debug_print;
    int debug_dump;
    bool mSeeking;
    //OMX_TICKS mEosTimeStamp;
    DECODER_STATETYPE  mDecoderState;
    Mutex mConfigLock;
    Mutex mSetUpLock;
    bool mSetUp;
    bool mIsFirst;
    bool mAbortPlaying;
    bool mIsSecure;
    unsigned char *mClearBuffer;
    int mClearLen;
    int64_t mNumFramesOutput;
    void initializeState_l();
    bool setUp();
    bool setUpAudioDecoder_l();
    bool load_ffmpeg_decoder_lib();
    bool unload_ffmpeg_decoder_lib();
    bool isSetUp();
    bool tearDown();
    bool tearDownAudioDecoder_l();
    bool passBufferToRenderer(void *inData, size_t inLen);
    void *gAmFFmpegCodecLibHandler;
    AmAudioCodec *mCodec;
    /*ffmpeg decoder lib function*/
    int (*ffmpeg_decoder_init)(const char *, AUDIO_INFO *,AmAudioCodec **);
    int (*ffmpeg_decoder_close)(AmAudioCodec *);
    int (*ffmpeg_decoder_process)(char * ,int ,int *,char *,int *,struct pcm_info *,AmAudioCodec *);
    int64_t mDecodeFilledTotalTimeUs;
    int64_t mDecoderFilledTimeUs;
    uint32_t mDecodingErrors;
    uint32_t mDecodedFrames;
    char sysfs_buf[BUFFER_MAX_LENGTH];
    char *mOutBuffer;
    int mOutBufferLen;
    int mOutSize;
};

}  // namespace android

#endif  // ANDROID_C2_AUDIO_FFMPEG_DECODER_H_
