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

#ifndef ANDROID_C2_AUDIO_EAC3_DECODER_H_
#define ANDROID_C2_AUDIO_EAC3_DECODER_H_

#include <C2AudioDecComponent.h>


struct AC3DecoderExternal;
struct AudioInfo;

#define BUFFER_MAX_LENGTH 128
#define AML_DEBUG_AUDIOINFO_REPORT_PROPERTY    "vendor.media.audio.info.report.debug"
#define REPORT_DECODED_INFO  "/sys/class/amaudio/codec_report_info"
#define DUMP_AUDIO_INFO_DECODE (0x1000)  //use to enable the audio report info prop

namespace android {

struct C2AudioEAC3Decoder : public C2AudioDecComponent {
    class IntfImpl;

    C2AudioEAC3Decoder(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2AudioEAC3Decoder();

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

    //add by amlogic
    struct pcm_info{
        int sample_rate;
        int channel_num;
        int bytes_per_sample;
        int bitstream_type;
    };
    struct pcm_info pcm_out_info;

private:
    enum {
        kNumDelayBlocksMax      = 8,
    };

    std::shared_ptr<IntfImpl> mIntf;

    bool mIsFirst;

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

    bool mAbortPlaying;
    char *spdif_addr;
    int16_t *mOutBuffer;
    char nAudioCodec; /**< AudioCodec.  1.ac3 2.eac3. */
    int digital_raw;
    int nIsEc3;
    AC3DecoderExternal *mConfig;
    Mutex mConfigLock;
    Mutex mSetUpLock;
    void *handle;
    bool mEos;
    bool mSetUp;
    bool adec_call;

    int64_t mAnchorTimeUs;
    int64_t decoder_offset;
    int64_t mNumFramesOutput;

    unsigned char *mRemainBuffer;
    int mRemainLen;
    int mRemainBufLen;

    void initializeState_l();
    bool setUp();
    bool setUpAudioDecoder_l();
    bool load_ddp_decoder_lib();
    bool load_license_decoder_lib(const char *filename);
    bool unload_ddp_decoder_lib(void);

    bool isSetUp();
    bool tearDown();
    bool tearDownAudioDecoder_l();
    //bool passBufferToRenderer(void *inData, size_t inLen);


    /*ddp decoder lib function*/
    int (*ddp_decoder_init)(int, int,void **);
    int (*ddp_decoder_cleanup)(void *);
    int (*ddp_decoder_process)(char * ,int ,int *,int ,char *,int *,struct pcm_info *,char *,int *,void *);
    void *gDDPDecoderLibHandler;
    uint32_t mDecodingErrors;
    uint32_t mTotalDecodedFrames;
    char sysfs_buf[BUFFER_MAX_LENGTH];
};

}  // namespace android

#endif  // ANDROID_C2_AUDIO_EAC3_DECODER_H_
