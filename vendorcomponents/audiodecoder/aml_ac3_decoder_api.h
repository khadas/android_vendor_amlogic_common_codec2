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

#ifndef AC3DECODER_API_H
#define AC3DECODER_API_H

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
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 500*1024
#define AUDIO_EXTRA_DATA_SIZE   (8192)

    typedef struct
#ifdef __cplusplus
                AC3DecoderExternal
#endif
    {
        uint8      *pInputBuffer;
        int32     inputBufferCurrentLength;
        int32     inputBufferUsedLength;
        uint32     CurrentFrameLength;
        //e_equalization     equalizerType;
        int32     inputBufferMaxLength;
        int16       num_channels;
        int16       version;
        int32       samplingRate;
        int32       bitRate;
        int32     outputFrameSize;
        int32     crcEnabled;
        uint32     totalNumberOfBitsUsed;
        int16       *pOutputBuffer;
        int32        debug_print;
        int32        debug_dump;
    } AC3DecoderExternal;

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

#endif /* AC3DECODER_API_H */
