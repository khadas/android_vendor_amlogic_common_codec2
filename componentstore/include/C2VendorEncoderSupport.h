/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
#ifndef ANDROID_CODEC2_VDA_ENCODER_SUPPORT_H
#define ANDROID_CODEC2_VDA_ENCODER_SUPPORT_H

#include <memory>
#include <C2Component.h>
#include <C2VendorSupport.h>

namespace android {

/*encoder*/
const C2String kH264EncoderName = "c2.amlogic.avc.encoder";
const C2String kH265EncoderName = "c2.amlogic.hevc.encoder";

static C2VendorComponent gC2VideoEncoderComponents [] = {
    {kH264EncoderName, C2VendorCodec::VENC_H264},
    {kH265EncoderName, C2VendorCodec::VENC_H265},
};

}  // namespace android

#endif  // ANDROID_CODEC2_VDA_ENCODER_SUPPORT_H

