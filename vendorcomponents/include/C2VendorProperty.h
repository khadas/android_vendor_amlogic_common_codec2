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

#ifndef _C2_VENDOR_PROPERTY_H
#define _C2_VENDOR_PROPERTY_H

/* log level */
#define CODEC2_LOGDEBUG_PROPERTY                    "vendor.media.c2.loglevels"
#define CODEC2_VDEC_LOGDEBUG_PROPERTY               CODEC2_LOGDEBUG_PROPERTY
#define CODEC2_VENC_LOGDEBUG_PROPERTY               CODEC2_LOGDEBUG_PROPERTY
#define CODEC2_ADEC_LOGDEBUG_PROPERTY               CODEC2_LOGDEBUG_PROPERTY

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
#define C2_PROPERTY_VDEC_INST_MAX_HIGH_RES_NUM      "vendor.media.c2.vdec.inst.max_high_res"


#define C2_PROPERTY_VDEC_OUT_DELAY                  "vendor.media.c2.vdec.out.delay"
#define C2_PROPERTY_VDEC_OUT_BUF_REALLOC            "vendor.media.c2.vdec.out.buf_realloc"

#define C2_PROPERTY_VDEC_DISP_NR_ENABLE             "vendor.media.c2.disp.nr.enable"
#define C2_PROPERTY_VDEC_DISP_NR_8K_ENABLE          "vendor.media.c2.disp.nr.8k_enable"
#define C2_PROPERTY_VDEC_DISP_DI_LOCALBUF_ENABLE    "vendor.media.c2.disp.di.localbuf_enable"

#define C2_PROPERTY_VDEC_DOUBLEWRITE                "vendor.media.c2.vdec.doublewrite"
#define C2_PROPERTY_VDEC_TRIPLEWRITE                "vendor.media.c2.vdec.triplewrite"
#define C2_PROPERTY_VDEC_ENABLE_AVC_4K_MMU          "vendor.media.c2.vdec.enable_h264_4k_mmu"
#define C2_PROPERTY_VDEC_AVC_MMU_WIDTH              "vendor.media.c2.vdec.h264_mmu_width"
#define C2_PROPERTY_VDEC_AVC_MMU_HEIGHT             "vendor.media.c2.vdec.h264_mmu_height"
#define C2_PROPERTY_VDEC_MARGIN                     "vendor.media.c2.vdec.margin"
#define C2_PROPERTY_VDEC_HDR_LITTLE_ENDIAN_ENABLE   "vendor.media.c2.vdec.hdr.little_endian_enable"
#define C2_PROPERTY_VDEC_ERRPOLICY_DISABLE          "vendor.media.c2.vdec.errpolicy_disable"
#define C2_PROPERTY_VDEC_REALLOC_TUNNEL_RESCHANGE   "vendor.media.c2.vdec.realloc_for_tunnel_reschange"
#define C2_PROPERTY_VDEC_RETRYBLOCK_TIMEOUT         "vendor.media.c2.vdec.retryblock_timeout"
#define C2_PROPERTY_VDEC_FORCE_DI_PERMISSION        "vendor.media.c2.vdec.force_di_permission"
#define C2_PROPERTY_VDEC_GAME_LOW_LATENCY           "vendor.media.c2.vdec.game_low_latency"
#define C2_PROPERTY_VDEC_FIXED_BUFF_SLICE           "vendor.media.common.fixed_buffer_slice"
#define C2_PROPERTY_VDEC_DI_POST                    "vendor.media.c2.vdec.di.post"
#define C2_PROPERTY_VDEC_DIABLE_VPP_THRES           "vendor.media.c2.vdec.disable_vpp_thres"
#define C2_PROPERTY_VDEC_AMDV_USE_P010                "vendor.media.c2.vdec.amdv_use_P010"


/* debug */
#define C2_PROPERTY_VDEC_FD_INFO_DEBUG              "debug.vendor.media.c2.vdec.fd_info_debug"
#define C2_PROPERTY_VDEC_SUPPORT_10BIT              "debug.vendor.media.c2.vdec.support_10bit"
#define C2_PROPERTY_VDEC_DISABLE_RC                 "debug.vendor.media.c2.vdec.disable-rc"
#define C2_PROPERTY_VDEC_DEBUG_PRIORITY             "debug.vendor.media.c2.vdec.priority"
#define C2_PROPERTY_VDEC_OUT_ADD_DELAY              "debug.vendor.media.c2.vdec.out.add_delay"
#define C2_PROPERTY_VDEC_DIABLE_BYPASS_VPP          "debug.vendor.media.c2.vdec.default.bypass_vpp"
#define C2_PROPERTY_VDEC_LLV_FIRST_ALLOC_NUM        "debug.vendor.media.c2.vdec.llv_first_alloc_num"

/* soft vdec */
#define C2_PROPERTY_SOFTVDEC_INST_MAX_NUM           "vendor.media.c2.softvdec.inst.max_num"
#define C2_PROPERTY_SOFTVDEC_DUMP_YUV               "debug.vendor.media.c2.softvdec.dump_yuv"

/* venc */

/* audio decoder */
#define C2_PROPERTY_AUDIO_DECODER_DEBUG             "vendor.media.c2.audio.decoder.debug"
#define C2_PROPERTY_AUDIO_DECODER_DUMP              "vendor.media.c2.audio.decoder.dump"


/* platform */
#define PROPERTY_PLATFORM_SUPPORT_4K                "ro.vendor.platform.support.4k"
#define PROPERTY_PLATFORM_SUPPORT_4K_FPS_MAX        "ro.vendor.platform.support.4k_fps_max"
#define PROPERTY_PLATFORM_SUPPORT_8K                "ro.vendor.platform.support.8k"
#define PROPERTY_PLATFORM_SUPPORT_HARDWARE_P010     "ro.vendor.platform.support.hardware_10bit"
#define PROPERTY_PLATFORM_SUPPORT_SOFTWARE_P010     "ro.vendor.platform.support.software_10bit"
#define PROPERTY_PLATFORM_SUPPORT_8K_BUF_MODE       "ro.vendor.platform.support.8k.buffer_mode"

/* vdec property set */
#define C2_PROPERTY_COMMON_LOWLATENCY_MODE          "vendor.media.llp"

#endif
