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

#ifndef ANDROID_C2_AAC_DEC_H_
#define ANDROID_C2_AAC_DEC_H_

#include <C2AudioDecComponent.h>

#include "aacdecoder_lib.h"

namespace android {

struct C2AudioAacDecoder : public C2AudioDecComponent {
    class IntfImpl;

    C2AudioAacDecoder(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2AudioAacDecoder();

    // From C2AudioDecComponent
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

    HANDLE_AACDECODER mAACDecoder;
    CStreamInfo *mStreamInfo;
    bool mIsFirst;
    size_t mInputBufferCount;
    size_t mOutputBufferCount;
    bool mSignalledError;
    size_t mOutputPortDelay;
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

    void drainRingBuffer(
            const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2BlockPool> &pool,
            bool eos);
    c2_status_t drainInternal(
            uint32_t drainMode,
            const std::shared_ptr<C2BlockPool> &pool,
            const std::unique_ptr<C2Work> &work);

//      delay compensation

    int32_t mOutputDelayCompensated;
    int32_t mOutputDelayRingBufferSize;
    std::unique_ptr<short[]> mOutputDelayRingBuffer;
    int32_t mOutputDelayRingBufferWritePos;
    int32_t mOutputDelayRingBufferReadPos;
    int32_t mOutputDelayRingBufferFilled;
    bool outputDelayRingBufferPutSamples(INT_PCM *samples, int numSamples);
    int32_t outputDelayRingBufferGetSamples(INT_PCM *samples, int numSamples);
    int32_t outputDelayRingBufferSamplesAvailable();
    int32_t outputDelayRingBufferSpaceLeft();
    uint32_t maskFromCount(uint32_t channelCount);

    C2_DO_NOT_COPY(C2AudioAacDecoder);
};

}  // namespace android

#endif  // ANDROID_C2_AAC_DEC_H_
