/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */

#ifndef ANDROID_CODEC2_VDA_SOFT_VIDEO_SUPPORT_H
#define ANDROID_CODEC2_VDA_SOFT_VIDEO_SUPPORT_H

#include <memory>
#include <C2Component.h>
#include <C2VendorSupport.h>

namespace android {

/* soft decoder */
const C2String kVP6ADecoderName = "c2.amlogic.vp6a.decoder.sw";
const C2String kVP6FDecoderName = "c2.amlogic.vp6f.decoder.sw";
const C2String kVP8DecoderName = "c2.amlogic.vp8.decoder.sw";
const C2String kH263DecoderName = "c2.amlogic.h263.decoder.sw";
const C2String kRM10DecoderName = "c2.amlogic.rm10.decoder.sw";
const C2String kRM20DecoderName = "c2.amlogic.rm20.decoder.sw";
const C2String kRM30DecoderName = "c2.amlogic.rm30.decoder.sw";
const C2String kRM40DecoderName = "c2.amlogic.rm40.decoder.sw";
const C2String kWMV1DecoderName = "c2.amlogic.wmv1.decoder.sw";
const C2String kWMV2DecoderName = "c2.amlogic.wmv2.decoder.sw";
const C2String kWMV3DecoderName = "c2.amlogic.wmv3.decoder.sw";
const C2String kVC1DecoderName = "c2.amlogic.vc1.decoder.sw";

#ifdef SUPPORT_SOFT_VDEC
static C2VendorComponent gC2SoftVideoDecoderComponents [] = {
    {kVP6ADecoderName, C2VendorCodec::VDEC_VP6A},
    {kVP6FDecoderName, C2VendorCodec::VDEC_VP6F},
    {kVP8DecoderName, C2VendorCodec::VDEC_VP8},
    {kH263DecoderName, C2VendorCodec::VDEC_H263},
    {kRM10DecoderName, C2VendorCodec::VDEC_RM10},
    {kRM20DecoderName, C2VendorCodec::VDEC_RM20},
    {kRM30DecoderName, C2VendorCodec::VDEC_RM30},
    {kRM40DecoderName, C2VendorCodec::VDEC_RM40},
    {kWMV1DecoderName, C2VendorCodec::VDEC_WMV1},
    {kWMV2DecoderName, C2VendorCodec::VDEC_WMV2},
    {kWMV3DecoderName, C2VendorCodec::VDEC_WMV3},
    {kVC1DecoderName, C2VendorCodec::VDEC_VC1},
};
#endif

}  // namespace android

#endif  // ANDROID_CODEC2_VDA_SOFT_VIDEO_SUPPORT_H

