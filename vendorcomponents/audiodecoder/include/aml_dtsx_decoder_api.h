#ifndef DTSXDECODER_API_H
#define DTSXDECODER_API_H

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
#define DTSX_PARAM_STRING_LEN 64
#define DTSX_PARAM_COUNT_MAX 64

    typedef struct
#ifdef __cplusplus
                DTSXDecoderExternal
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
        ///< Control
    void *p_dtsx_dec_inst;
    void *p_dtsx_pp_inst;

    ///< Information
    int status;
    int remain_size;
    int half_frame_remain_size;
    int half_frame_used_size;
    unsigned int outlen_pcm;
    unsigned int outlen_raw;

    char *core1_out_pcm;
    int core1_out_buff_size;
    ///< Parameter
    char *init_argv[DTSX_PARAM_COUNT_MAX];
    int init_argc;
    bool is_dtscd;
    bool is_iec61937;
    int sink_dev_type;
    int passthroug_enable;
    int auto_config_out_for_vx;
    int loudness_enable;
    unsigned char *inbuf;
    unsigned int inbuf_size;
    unsigned char *a_dtsx_pp_output[3];
    unsigned int a_dtsx_pp_output_size[3];
    int is_hdmi_output;
    } DTSXDecoderExternal;

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

#endif /* DTSXDECODER_API_H */