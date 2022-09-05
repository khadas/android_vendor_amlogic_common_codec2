/*
 * Copyright (c) 2014 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
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
