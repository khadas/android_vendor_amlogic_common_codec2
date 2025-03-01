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

#ifndef VIDEODECWRAPER_H_
#define VIDEODECWRAPER_H_

#include <AmVideoDecBase.h>
#include <video_decode_accelerator.h>
#include "AmlMessageBase.h"

namespace android {


// This class translates adaptor API to media::VideoDecodeAccelerator API to make communication
// between Codec 2.0 VDA component and VDA.
class VideoDecWraper :        public AmVideoDecCallback {
public:

    class VideoDecWraperCallback {
    public:
        virtual ~VideoDecWraperCallback() {}

        virtual void ProvidePictureBuffers(uint32_t minNumBuffers,  uint32_t width, uint32_t height)  = 0;
        virtual void DismissPictureBuffer(int32_t picture_buffer_id)  = 0;
        virtual void PictureReady(int32_t pictureBufferId, int64_t bitstreamId,
                                      uint32_t x, uint32_t y, uint32_t w, uint32_t h, int32_t flags)  = 0;
        virtual void PictureReady(output_buf_param_t* params)  = 0;

        virtual void UpdateDecInfo(const uint8_t* info, uint32_t isize) = 0;
        virtual void NotifyEndOfBitstreamBuffer(int32_t bitstream_buffer_id)  = 0;
        virtual void NotifyFlushDone()  = 0;
        virtual void NotifyFlushOrStopDone()  = 0;
        virtual void NotifyError(int error)  = 0;
        virtual void NotifyEvent(uint32_t event, void* param, uint32_t paramsize) = 0;

   };

    enum Event_v4l2 {
        // Event file is a interlaced
        FIELD_INTERLACED = 1,
        FRAME_ERROR = 3,
        FARAME_INCOMPLETE = 4,
    };

    VideoDecWraper();
    ~VideoDecWraper();
    static bool loadMediaHalLibrary(void);
    static media::VideoDecodeAccelerator::SupportedProfiles AmVideoDec_getSupportedProfiles(uint32_t inputcodec);
    static uint32_t AmVideoDec_getResolveBufferFormat(bool crcb, bool semiplanar);
    static AmlMessageBase* AmVideoDec_getAmlMessage();

    // Implementation of the VideoDecodeAcceleratorAdaptor interface.
    int initialize(const char* mime, uint8_t* config, uint32_t configLen,
                                bool secureMode, VideoDecWraperCallback* client, int32_t flags = 0,  char * resAppName = NULL, void (* resmCallback)(void *) = NULL, void *resOpaque = NULL);
    int32_t decode(int32_t bitstreamId, uint8_t* pbuf, off_t offset, uint32_t bytesUsed, uint64_t timestamp, uint8_t* hdrbuf = NULL, uint32_t hdrlen = 0, int32_t flags = 0);
    int32_t decode(int32_t bitstreamId, int ashmemFd, off_t offset, uint32_t bytesUsed, uint64_t timestamp, uint8_t* hdrbuf = NULL, uint32_t hdrlen = 0, int32_t flags = 0);
    void assignPictureBuffers(uint32_t numOutputBuffers);
    void importBufferForPicture(int32_t pictureBufferId,
                                int fd,
                                int metafd,
                                uint8_t* buf,
                                size_t size,
                                bool isNV21);
    void reusePictureBuffer(int32_t pictureBufferId);
    int32_t allocTunnelBuffer(int usage, uint32_t format, int stride, uint32_t width, uint32_t height, bool secure, int* fd);
    int32_t freeTunnelBuffer(int fd);
    void eosFlush();
    void reset(uint32_t flags = 0);
    void flush(uint32_t flags = 0);
    void stop(uint32_t flags = 0);
    void destroy();
    bool postAndReplyMsg(AmlMessageBase *msg);
    void setSessionID(int32_t id);
    int32_t getDecoderID();
    void setPipeLineWorkNumber(uint32_t number);
    void setOutputFormat(int32_t output_format);
    //media::VideoDecodeAccelerator::SupportedProfiles GetSupportedProfiles(InputCodec inputCodec);

    // Implementation of the media::VideoDecodeAcceleratorAdaptor::Client interface.
    virtual void onOutputFormatChanged(uint32_t requested_num_of_buffers,
                int32_t width, uint32_t height);
    virtual void onOutputBufferDone(int32_t pictureBufferId, int64_t bitstreamId,
                uint32_t width, uint32_t height);
    virtual void onOutputBufferDone(int32_t pictureBufferId, int64_t bitstreamId,
            uint32_t width, uint32_t height, int32_t flags);
    virtual void onOutputBufferDone(output_buf_param_t* params);

    virtual void onInputBufferDone(int32_t bitstream_buffer_id);
    virtual void onUserdataReady(const uint8_t* userdata, uint32_t usize);
    virtual void onUpdateDecInfo(const uint8_t* info, uint32_t isize);
    virtual void onFlushDone();
    virtual void onResetDone();
    virtual void onError(int32_t error);
    virtual void onEvent(uint32_t event, void* param, uint32_t paramsize);

private:

    void setSessionID2Hal();
    void setPipelineWorkNumber2Hal();
    AmVideoDecBase* mAmVideoDec;
    VideoDecWraperCallback* mDecoderCallback;
    int32_t mSessionID;
    int32_t mDecoderID;
    uint32_t mPipeLineWorkNum;
};

enum {
    /* metadata_config_flag */
    VDEC_CFG_FLAG_DV_TWOLAYER = (1<<0),
    VDEC_CFG_FLAG_DV_NEGATIVE  = (1<<1),
    VDEC_CFG_FLAG_DISABLE_DECODE_VPP = (1<<8),
    VDEC_CFG_FLAG_DYNAMIC_BYPASS_DI = (1<<10),
    VDEC_CFG_FLAG_DIS_ERR_POLICY = (1 << 11),
    VDEC_CFG_FLAG_DI_LOCALBUF_ENABLE = (1<<14),
    VDEC_CFG_FLAG_NR_ENABLE    = (1<<15),
    VDEC_CFG_FLAG_PROG_ONLY    = (1<<16),
    VDEC_CFG_FLAG_FORCE_DI     = (1<<17),
    VDEC_CFG_FLAG_RELEASE_VPP_EARLY = (1<<18),
    VDEC_CFG_FLAG_UNUSE_AVBC_OUT = (1<<19),
    VDEC_CFG_FLAG_DI_POST      = (1<<20),
    VDEC_CFG_FLAG_BUF_MODE      = (1<<21),
};

enum {
    /* buf mode */
    DMA_BUF_MODE = 0,
    ION_BUF_MODE = 1,
};


enum {
    /* low_latency_mode */
    LOWLATENCY_DISABLE,
    LOWLATENCY_NORMAL  = (1 << 0),
    LOWLATENCY_FENCE = (1 << 1),
};

enum {
    /* update config type */
    INTERLACE = 1 << 0,
    DOUBLE_WRITE = 1 << 1,
    TUNNEL_UNDERFLOW = 1 << 2,
    YCBCR_P010_STREAM = 1 << 3,
};

struct aml_vdec_cfg_infos {
    uint32_t double_write_mode;
    uint32_t init_width;
    uint32_t init_height;
    uint32_t ref_buf_margin;
    uint32_t canvas_mem_mode;
    uint32_t canvas_mem_endian;
    uint32_t low_latency_mode;
    uint32_t uvm_hook_type;
    uint32_t metadata_config_flag;
    uint32_t duration;
    uint32_t triple_write_mode;
    uint32_t dv_profile;
    uint32_t data[2];
};
struct aml_vdec_ps_infos {
    uint32_t visible_width;
    uint32_t visible_height;
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t profile;
    uint32_t mb_width;
    uint32_t mb_height;
    uint32_t dpb_size;
    uint32_t ref_frames;
    uint32_t dpb_frames;
    uint32_t dpb_margin;
    uint32_t field;
    uint32_t data[3];
};

struct aml_vdec_cnt_infos {
    uint32_t bit_rate;
    uint32_t frame_count;
    uint32_t error_frame_count;
    uint32_t drop_frame_count;
    uint32_t total_data;
};


struct vframe_content_light_level_s {
	uint32_t present_flag;
	uint32_t max_content;
	uint32_t max_pic_average;
}; /* content_light_level from SEI */

struct vframe_master_display_colour_s {
	uint32_t present_flag;
	uint32_t primaries[3][2];
	uint32_t white_point[2];
	uint32_t luminance[2];
	struct vframe_content_light_level_s content_light_level;
}; /* master_display_colour_info_volume from SEI */

struct aml_vdec_hdr_infos {
	/*
	 * bit 29   : present_flag
	 * bit 28-26: video_format "component", "PAL", "NTSC", "SECAM", "MAC", "unspecified"
	 * bit 25   : range "limited", "full_range"
	 * bit 24   : color_description_present_flag
	 * bit 23-16: color_primaries "unknown", "bt709", "undef", "bt601",
	 *            "bt470m", "bt470bg", "smpte170m", "smpte240m", "film", "bt2020"
	 * bit 15-8 : transfer_characteristic unknown", "bt709", "undef", "bt601",
	 *            "bt470m", "bt470bg", "smpte170m", "smpte240m",
	 *            "linear", "log100", "log316", "iec61966-2-4",
	 *            "bt1361e", "iec61966-2-1", "bt2020-10", "bt2020-12",
	 *            "smpte-st-2084", "smpte-st-428"
	 * bit 7-0  : matrix_coefficient "GBR", "bt709", "undef", "bt601",
	 *            "fcc", "bt470bg", "smpte170m", "smpte240m",
	 *            "YCgCo", "bt2020nc", "bt2020c"
	 */
	uint32_t signal_type;
	struct vframe_master_display_colour_s color_parms;
};

/* types of decode parms. */
#define V4L2_CONFIG_PARM_DECODE_CFGINFO	(1 << 0)
#define V4L2_CONFIG_PARM_DECODE_PSINFO	(1 << 1)
#define V4L2_CONFIG_PARM_DECODE_HDRINFO	(1 << 2)
#define V4L2_CONFIG_PARM_DECODE_CNTINFO	(1 << 3)

struct aml_dec_params {
    uint32_t parms_status;
    struct aml_vdec_cfg_infos cfg;
    struct aml_vdec_ps_infos  ps;
    struct aml_vdec_hdr_infos hdr;
    struct aml_vdec_cnt_infos cnt;
};

struct v4l2_parms {
    uint32_t magic;
    uint32_t len;
    uint32_t adaptivePlayback;
    uint32_t height;
    uint32_t width;
};

struct mediahal_cfg_parms {
    struct v4l2_parms v4l2_cfg;
    struct aml_dec_params aml_dec_cfg;
};

}
#endif
