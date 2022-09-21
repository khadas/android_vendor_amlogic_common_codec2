/*
 * Copyright (c) 2014 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
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
