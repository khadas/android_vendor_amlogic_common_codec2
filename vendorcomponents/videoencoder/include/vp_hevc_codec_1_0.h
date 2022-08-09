#ifndef _INCLUDED_COM_VIDEOPHONE_CODEC
#define _INCLUDED_COM_VIDEOPHONE_CODEC

#ifdef __cplusplus
extern "C" {
#endif

#define vl_codec_handle_t long

    typedef enum vl_codec_id {
        CODEC_ID_NONE,
        CODEC_ID_VP8,
        CODEC_ID_H261,
        CODEC_ID_H263,
        CODEC_ID_H264, /* must support */
        CODEC_ID_H265,
    } vl_codec_id_t;

    typedef enum vl_img_format
    {
        IMG_FMT_NONE,
        IMG_FMT_NV12, /* must support  */
        IMG_FMT_NV21,
        IMG_FMT_YV12,
        IMG_FMT_RGB888,
        IMG_FMT_BGR888,
        IMG_FMT_RGBA8888,
    } vl_img_format_t;

    typedef enum vl_frame_type
    {
        FRAME_TYPE_NONE,
        FRAME_TYPE_AUTO, /* encoder self-adaptation(default) */
        FRAME_TYPE_IDR,
        FRAME_TYPE_I,
        FRAME_TYPE_P,
    } vl_frame_type_t;

    typedef enum vl_buffer_type
    {
        VMALLOC_BUFFER_TYPE = 0,
        CANVAS_BUFFER_TYPE = 1,
        PHYSICAL_BUFF_TYPE = 2,
        DMA_BUFF_TYPE = 3,
        MAX_TYPE_TYPE = 4,
    } vl_buffer_type_e;

    typedef enum vl_h_enc_result
    {
        ENC_TIMEOUT = -3,
        ENC_SKIPPED_PICTURE,
        ENC_FAILED,
        ENC_SUCCESS,
        ENC_IDR_FRAME,
    }vl_enc_result_e;

    typedef enum vl_h_enc_csc {
        ENC_CSC_BT601,
        ENC_CSC_BT709
    }vl_h_enc_csc_e;

    typedef enum vl_h_enc_profile{
        ENC_HEVC_BASELINE = 66,
        ENC_HEVC_MAIN = 77,
        ENC_HEVC_EXTENDED = 88,
        ENC_HEVC_HIGH = 100,
        ENC_HEVC_HIGH10 = 110,
        ENC_HEVC_HIGH422 = 122,
        ENC_HEVC_HIGH444 = 144
    } vl_h_enc_profile_e;

    typedef enum vl_h_enc_level{
        ENC_HEVC_LEVEL_AUTO = 0,
        ENC_HEVC_LEVEL1_B = 9,
        ENC_HEVC_LEVEL1 = 10,
        ENC_HEVC_LEVEL1_1 = 11,
        ENC_HEVC_LEVEL1_2 = 12,
        ENC_HEVC_LEVEL1_3 = 13,
        ENC_HEVC_LEVEL2 = 20,
        ENC_HEVC_LEVEL2_1 = 21,
        ENC_HEVC_LEVEL2_2 = 22,
        ENC_HEVC_LEVEL3 = 30,
        ENC_HEVC_LEVEL3_1 = 31,
        ENC_HEVC_LEVEL3_2 = 32,
        ENC_HEVC_LEVEL4 = 40,
        ENC_HEVC_LEVEL4_1 = 41,
        ENC_HEVC_LEVEL4_2 = 42,
        ENC_HEVC_LEVEL5 = 50,
        ENC_HEVC_LEVEL5_1 = 51
    } vl_h_enc_level_e;


    typedef struct vl_vui_params {
        bool vui_info_present;
        int primaries;
        int transfer;
        int matrixCoeffs;
        unsigned char range;
    }vl_vui_params_t;

    typedef struct vl_init_params
    {
        int width;
        int height;
        int frame_rate;
        int bit_rate;
        int gop;
        int i_qp_min;
        int i_qp_max;
        int p_qp_min;
        int p_qp_max;
        vl_h_enc_csc_e csc;
        vl_h_enc_profile_e profile;
        vl_h_enc_level_e level;
        vl_vui_params_t vui_info;
    }vl_init_params_t;

    typedef struct vl_frame_info
    {
        unsigned long YCbCr[3];
        vl_buffer_type_e type;
        vl_frame_type_t frame_type;
        unsigned long frame_size;
        vl_img_format_t fmt; //0:nv12 1:nv21 2:YUV420 3:rgb888 4:bgr888
        int pitch;
        int height;
        unsigned int coding_timestamp;
        unsigned int canvas;
        unsigned int scale_width;
        unsigned int scale_height;
        unsigned int crop_left;
        unsigned int crop_right;
        unsigned int crop_top;
        unsigned int crop_bottom;
        unsigned int bitrate;
    } vl_frame_info_t;

    typedef enum vl_error_type_e
    {
        ERR_HARDWARE = -4,
        ERR_OVERFLOW = -3,
        ERR_NOTSUPPORT = -2,
        ERR_UNDEFINED = -1,
    } vl_error_type_e;

    /**
     * Getting version information
     *
     *@return : version information
     */
    const char *vl_get_version();

    /**
     * init encoder
     *
     *@param : codec_id: codec type
     *@param : width: video width
     *@param : height: video height
     *@param : frame_rate: framerate
     *@param : bit_rate: bitrate
     *@param : gop GOP: max I frame interval
     *@return : if success return encoder handle,else return <= 0
     */
    vl_codec_handle_t vl_video_encoder_init(vl_codec_id_t codec_id, vl_init_params_t initParam);


    /**
     * encode video header
     *
     *@param : handle
     *@param : in_size: data size
     *@param : out: data output,H.264 need header(0x00，0x00，0x00，0x01),and format must be I420(apk set param out，through jni,so modify "out" in the function,don't change address point)
     *@return ：if success return encoded data length,else return <= 0
     */
    int vl_video_encode_header(vl_codec_handle_t codec_handle, int in_size, unsigned char *out);
    /**
     * encode video
     *
     *@param : handle
     *@param : type: frame type
     *@param : in: data to be encoded
     *@param : in_size: data size
     *@param : out: data output,HEVC need header(0x00，0x00，0x00，0x01),and format must be I420(apk set param out，through jni,so modify "out" in the function,don't change address point)
     *@return ：if success return encoded data length,else return error
     */
    int vl_video_encoder_encode(vl_codec_handle_t codec_handle, vl_frame_info_t frameinfo,unsigned int outputBufferLen, unsigned char *out, vl_frame_type_t *frameType);

    /**
     * destroy encoder
     *
     *@param ：handle: encoder handle
     *@return ：if success return 1,else return 0
     */
    int vl_video_encoder_destory(vl_codec_handle_t handle);

    /**
     * init decoder
     *
     *@param : codec_id: decoder type
     *@return : if success return decoder handle,else return <= 0
     */
//    vl_codec_handle_t vl_video_decoder_init(vl_codec_id_t codec_id);

    /**
     * decode video
     *
     *@param : handle: decoder handle
     *@param : in: data to be decoded
     *@param : in_size: data size
     *@param : out: data output, intenal set
     *@return ：if success return decoded data length, else return <= 0
     */
//    int vl_video_decoder_decode(vl_codec_handle_t handle, char *in, int in_size, char **out);

    /**
     * destroy decoder
     *@param : handle: decoderhandle
     *@return ：if success return 1, else return 0
     */
//    int vl_video_decoder_destory(vl_codec_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* _INCLUDED_COM_VIDEOPHONE_CODEC */
