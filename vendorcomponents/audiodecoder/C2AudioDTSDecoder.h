/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef ANDROID_C2_AUDIO_DTS_DECODER_H_
#define ANDROID_C2_AUDIO_DTS_DECODER_H_

#include <C2AudioDecComponent.h>

struct DTSDecoderExternal;
struct AudioInfo;

namespace android {


struct C2AudioDTSDecoder : public C2AudioDecComponent {

    class IntfImpl;

    C2AudioDTSDecoder(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2AudioDTSDecoder();

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

    struct Info {
        uint64_t frameIndex;
        size_t bufferSize;
        uint64_t timestamp;
        std::vector<int32_t> decodedSizes;
    };
    std::list<Info> mBuffersInfo;

    enum {
        NONE,
        AWAITING_DISABLED,
        AWAITING_ENABLED
    } mOutputPortSettingsChange;

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

    // === Add by amlogic start ===
    bool mInputFlushing;
    bool mOutputFlushing;
    bool mInputFlushDone;
    bool mOutputFlushDone;
    char *mOutputBuffer;
    char *mOutputRawBuffer;
    bool mRunning;
    Mutex mFlushLock;
    DTSDecoderExternal *mConfig;
    Mutex mConfigLock;
    Mutex mSetUpLock;
    bool mEos;
    bool mSetUp;
    bool mIsFirst;
    bool mAbortPlaying;
    int  nPassThroughEnable;
    int decode_offset;
    bool adec_call;

    void initializeState_l();
    bool setUp();
    bool setUpAudioDecoder_l();
    bool load_dts_decoder_lib(const char *filename);
    bool unload_dts_decoder_lib();
    bool isSetUp();
    bool tearDown();
    bool tearDownAudioDecoder_l();

    /*dts decoder lib function*/
    int (*dts_decoder_init)(int, int);
    int (*dts_decoder_cleanup)();
    int (*dts_decoder_process)(char * ,int ,int *,char *,int *,struct pcm_info *,char *,int *);
    void *gDtsDecoderLibHandler;
    // === Add by amlogic end ===
};

}  // namespace android

#endif  // ANDROID_C2_AUDIO_DTS_DECODER_H_
