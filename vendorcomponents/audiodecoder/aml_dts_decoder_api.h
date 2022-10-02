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

#ifndef DTSDECODER_API_H
#define DTSDECODER_API_H

#ifdef __cplusplus
extern "C"
{
#endif

typedef int8_t int8;
typedef uint8_t uint8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef int32_t int32;
typedef uint32_t uint32;
typedef int64_t int64;
typedef uint64_t uint64;
typedef int32_t Int32;
#define AVCODEC_MAX_AUDIO_FRAME_SIZE (500*1024)
#define AUDIO_EXTRA_DATA_SIZE   (8192)

    typedef struct
#ifdef __cplusplus
                DTSDecoderExternal
#endif
    {
        int32      bitrate;
        int32      samplerate;
        int32      channels;
        int32      digital_raw;
        char      *pInput;
        char      *pOutput;
        char      *poutput_pcm;
        char      *poutput_raw;
        int32      inputlen;
        int32      inputlen_used;
        int32      inputlen_max;
        int32      outputlen_pcm;
        int32      outputlen_raw;
        int32      outputlen_frames;
        int32      outputlen;
        int32      debug_print;
        int32      debug_dump;
        void *pDecoderLibHandler;
        void *decoder_init;
        void *decoder_cleanup;
        void *decoder_process;
    } DTSDecoderExternal;

    typedef struct
#ifdef __cplusplus
                AudioInfo
#endif
    {
        int bitrate;
        int samplerate;
        int channels;
        int file_profile;
    } AudioInfo;

#ifdef __cplusplus
}
#endif

#endif /* DTSDECODER_API_H */
