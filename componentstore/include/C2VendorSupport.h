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

#ifndef ANDROID_CODEC2_VDA_SUPPORT_H
#define ANDROID_CODEC2_VDA_SUPPORT_H

#include <memory>
#include <C2Component.h>
namespace android {

enum RETURN_STATUS {
    RET_FAIL = -1,
    RET_OK = 0,
    UNKNOWN = 0xff,
};

enum class C2VendorCodec {
    UNKNOWN,
    VDEC_H264,
    VDEC_H265,
    VDEC_VP9,
    VDEC_AV1,
    VDEC_DVHE,
    VDEC_DVAV,
    VDEC_DVAV1,
    VDEC_MP2V,
    VDEC_MP4V,
    VDEC_MJPG,
    VDEC_AVS3,
    VDEC_AVS2,
    VDEC_AVS,
    VDEC_HW_VC1,
    VDEC_TYPE_MAX = VDEC_HW_VC1,
#ifdef SUPPORT_SOFT_VDEC
    VDEC_VP6A,
    VDEC_VP6F,
    VDEC_VP8,
    VDEC_H263,
    VDEC_RM10,
    VDEC_RM20,
    VDEC_RM30,
    VDEC_RM40,
    VDEC_WMV1,
    VDEC_WMV2,
    VDEC_WMV3,
    VDEC_VC1,
#endif
    VENC_H264,
    VENC_H265,
    ADEC_MP2 = 50,
    ADEC_AAC,
    ADEC_AC3,
    ADEC_EAC3,
    ADEC_FFMPEG, //54
    ADEC_DTS,
    ADEC_DTSHD,
    ADEC_DTSE,
    ADEC_AC4,
};

struct C2VendorComponent {
    std::string compname;
    C2VendorCodec codec;
};

/**
 * Returns the C2VDA component store.
 * \retval nullptr if the platform component store could not be obtained
 */
std::shared_ptr<C2ComponentStore> GetCodec2VendorComponentStore();
}  // namespace android

#endif  // ANDROID_CODEC2_VDA_SUPPORT_H
