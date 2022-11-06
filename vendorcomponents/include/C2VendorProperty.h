#ifndef _C2_VENDOR_PROPERTY_H
#define _C2_VENDOR_PROPERTY_H

/* debug log level */
#define CODEC2_VDEC_LOGDEBUG_PROPERTY               "debug.vendor.media.c2.vdec.loglevels"
#define CODEC2_VENC_LOGDEBUG_PROPERTY               "debug.vendor.media.c2.venc.loglevels"

/* store */
#define C2_PROPERTY_VDEC_SUPPORT                    "vendor.media.c2.vdec.support"
#define C2_PROPERTY_SOFTVDEC_SUPPORT                "vendor.media.c2.softvdec.support"
#define C2_PROPERTY_VDEC_SECURE_DISABLE             "vendor.media.c2.vdec.secure.disable"
#define C2_PROPERTY_VENC_SUPPORT                    "vendor.media.c2.venc.support"
#define C2_PROPERTY_ADEC_SUPPORT                    "vendor.media.c2.adec.support"
#define C2_PROPERTY_VENDOR_STORE_ENABLE             "debug.vendorstore.enable-c2"
#define C2_PROPERTY_VDEC_SUPPORT_FEATURELIST        "vendor.media.c2.vdec.support_featurelist"
#define C2_PROPERTY_VDEC_SUPPORT_MEDIACODEC         "vendor.media.c2.vdec.support_mediacodec_xml"

/* vdec */
#define C2_PROPERTY_VDEC_INPUT_DELAY_NUM_SECURE     "vendor.media.c2.vdec.input.delay_num_secure"
#define C2_PROPERTY_VDEC_INPUT_DELAY_NUM            "vendor.media.c2.vdec.input.delay_num"
#define C2_PROPERTY_VDEC_INPUT_MAX_SIZE             "vendor.media.c2.vdec.input.max_size"
#define C2_PROPERTY_VDEC_INPUT_MAX_PADDINGSIZE      "vendor.media.c2.vdec.input.max_paddingsize"

#define C2_PROPERTY_VDEC_INST_MAX_NUM               "vendor.media.c2.vdec.inst.max_num"
#define C2_PROPERTY_VDEC_INST_MAX_NUM_SECURE        "vendor.media.c2.vdec.inst.max_num_secure"

#define C2_PROPERTY_VDEC_OUT_DELAY                  "vendor.media.c2.vdec.out.delay"
#define C2_PROPERTY_VDEC_OUT_BUF_REALLOC            "vendor.media.c2.vdec.out.buf_realloc"

#define C2_PROPERTY_VDEC_DISP_NR_ENABLE             "vendor.media.c2.disp.nr.enable"
#define C2_PROPERTY_VDEC_DISP_NR_8K_ENABLE          "vendor.media.c2.disp.nr.8k_enable"
#define C2_PROPERTY_VDEC_DISP_DI_LOCALBUF_ENABLE    "vendor.media.c2.disp.di.loacalbuf_enable"

#define C2_PROPERTY_VDEC_DOUBLEWRITE                "vendor.media.c2.vdec.doublewrite"
#define C2_PROPERTY_VDEC_MARGIN                     "vendor.media.c2.vdec.margin"
#define C2_PROPERTY_VDEC_HDR_LITTLE_ENDIAN_ENABLE   "vendor.media.c2.vdec.hdr.little_endian_enable"
#define C2_PROPERTY_VDEC_ERRPOLICY_DISABLE          "vendor.media.c2.vdec.errpolicy_disable"
#define C2_PROPERTY_VDEC_REALLOC_TUNNEL_RESCHANGE   "vendor.media.c2.vdec.realloc_for_tunnel_reschange"
#define C2_PROPERTY_VDEC_RETRYBLOCK_TIMEOUT         "vendor.media.c2.vdec.retryblock_timeout"

//debug
#define C2_PROPERTY_VDEC_FD_INFO_DEBUG              "debug.vendor.media.c2.vdec.fd_info_debug"
#define C2_PROPERTY_VDEC_SUPPORT_10BIT              "debug.vendor.media.c2.vdec.support_10bit"

/* soft vdec */
#define C2_PROPERTY_SOFTVDEC_INST_MAX_NUM           "vendor.media.c2.softvdec.inst.max_num"
#define C2_PROPERTY_SOFTVDEC_DUMP_YUV               "debug.vendor.media.c2.softvdec.dump_yuv"

/* venc */

#endif
