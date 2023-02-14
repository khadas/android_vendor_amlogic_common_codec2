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

#ifndef AC4DECODER_API_H
#define AC4DECODER_API_H

#include "AmlAudioCommon.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
#ifdef __cplusplus
                AC4DecoderExternal
#endif
    {
        uint8 *pInputBuffer;
        int32 inputBufferCurrentLength;
        int32 inputBufferUsedLength;
        uint32 CurrentFrameLength;
        int32 inputBufferMaxLength;
        int16 num_channels;
        int16 version;
        int32 samplingRate;
        int32 bitRate;
        int32 nBytesPCMOut;
        int32 nByteCurrentPCMOut;
        int32 crcEnabled;
        uint32 totalNumberOfBitsUsed;
        uint8 *pOutputBuffer;
        int32 debug_print;
        int32 debug_dump;
        void *pDecoderLibHandler;
        void *decoder_init;
        void *decoder_cleanup;
        void *decoder_process;
    } AC4DecoderExternal;

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

#endif /* AC4DECODER_API_H */

