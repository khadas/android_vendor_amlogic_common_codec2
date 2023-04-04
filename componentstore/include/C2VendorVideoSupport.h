/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */

#ifndef ANDROID_CODEC2_VDA__VIDEO_SUPPORT_H
#define ANDROID_CODEC2_VDA__VIDEO_SUPPORT_H

#include <memory>
#include <C2Component.h>
#include <C2VendorSupport.h>
#include <AmVideoDecBase.h>

namespace android {

/*video*/
const C2String kH264DecoderName = "c2.amlogic.avc.decoder";
const C2String kH265DecoderName = "c2.amlogic.hevc.decoder";
const C2String kVP9DecoderName  = "c2.amlogic.vp9.decoder";
const C2String kAV1DecoderName  = "c2.amlogic.av1.decoder";
const C2String kMP2VDecoderName = "c2.amlogic.mpeg2.decoder";
const C2String kMP4VDecoderName = "c2.amlogic.mpeg4.decoder";
const C2String kMJPGDecoderName = "c2.amlogic.mjpeg.decoder";
const C2String kAVSDecoderName  = "c2.amlogic.avs.decoder";
const C2String kAVS2DecoderName = "c2.amlogic.avs2.decoder";
const C2String kAVS3DecoderName = "c2.amlogic.avs3.decoder";

/* secure */
const C2String kH264SecureDecoderName = "c2.amlogic.avc.decoder.secure";
const C2String kH265SecureDecoderName = "c2.amlogic.hevc.decoder.secure";
const C2String kVP9SecureDecoderName = "c2.amlogic.vp9.decoder.secure";
const C2String kAV1SecureDecoderName = "c2.amlogic.av1.decoder.secure";
const C2String kMP2VSecureDecoderName = "c2.amlogic.mpeg2.decoder.secure";
const C2String kMP4VSecureDecoderName = "c2.amlogic.mpeg4.decoder.secure";

/* dolby-vision */
const C2String kDVHEDecoderName = "c2.amlogic.dolby-vision.dvhe.decoder";
const C2String kDVAVDecoderName = "c2.amlogic.dolby-vision.dvav.decoder";
const C2String kDVAV1DecoderName = "c2.amlogic.dolby-vision.dav1.decoder";

const C2String kDVHESecureDecoderName = "c2.amlogic.dolby-vision.dvhe.decoder.secure";
const C2String kDVAVSecureDecoderName = "c2.amlogic.dolby-vision.dvav.decoder.secure";
const C2String kDVAV1SecureDecoderName = "c2.amlogic.dolby-vision.dav1.decoder.secure";

struct C2ComponentInputCodec {
    C2String compname;
    InputCodec codec;
};

static C2ComponentInputCodec gC2ComponentInputCodec [] = {
    {kH264DecoderName, InputCodec::H264},
    {kH264SecureDecoderName, InputCodec::H264},
    {kH265DecoderName, InputCodec::H265},
    {kH265SecureDecoderName, InputCodec::H265},
    {kVP9DecoderName, InputCodec::VP9},
    {kVP9SecureDecoderName, InputCodec::VP9},
    {kAV1DecoderName, InputCodec::AV1},
    {kAV1SecureDecoderName, InputCodec::AV1},
    {kDVHEDecoderName, InputCodec::DVHE},
    {kDVHESecureDecoderName, InputCodec::DVHE},
    {kDVAVDecoderName, InputCodec::DVAV},
    {kDVAVSecureDecoderName, InputCodec::DVAV},
    {kDVAV1DecoderName, InputCodec::DVAV1},
    {kDVAV1SecureDecoderName, InputCodec::DVAV1},
    {kMP2VDecoderName, InputCodec::MP2V},
    {kMP2VSecureDecoderName, InputCodec::MP2V},
    {kMP4VDecoderName, InputCodec::MP4V},
    {kMJPGDecoderName, InputCodec::MJPG},
    {kAVS3DecoderName, InputCodec::AVS3},
    {kAVS2DecoderName, InputCodec::AVS2},
    {kAVSDecoderName, InputCodec::AVS},
};


static C2VendorComponent gC2VideoDecoderComponents [] = {
    {kH264DecoderName, C2VendorCodec::VDEC_H264},
    {kH264SecureDecoderName, C2VendorCodec::VDEC_H264},
    {kH265DecoderName, C2VendorCodec::VDEC_H265},
    {kH265SecureDecoderName, C2VendorCodec::VDEC_H265},
    {kVP9DecoderName, C2VendorCodec::VDEC_VP9},
    {kVP9SecureDecoderName, C2VendorCodec::VDEC_VP9},
    {kAV1DecoderName, C2VendorCodec::VDEC_AV1},
    {kAV1SecureDecoderName, C2VendorCodec::VDEC_AV1},
    {kDVHEDecoderName, C2VendorCodec::VDEC_DVHE},
    {kDVHESecureDecoderName, C2VendorCodec::VDEC_DVHE},
    {kDVAVDecoderName, C2VendorCodec::VDEC_DVAV},
    {kDVAVSecureDecoderName, C2VendorCodec::VDEC_DVAV},
    {kDVAV1DecoderName, C2VendorCodec::VDEC_DVAV1},
    {kDVAV1SecureDecoderName, C2VendorCodec::VDEC_DVAV1},
    {kMP2VDecoderName, C2VendorCodec::VDEC_MP2V},
    {kMP2VSecureDecoderName, C2VendorCodec::VDEC_MP2V},
    {kMP4VDecoderName, C2VendorCodec::VDEC_MP4V},
    {kMJPGDecoderName, C2VendorCodec::VDEC_MJPG},
#ifdef SUPPORT_VDEC_AVS3
    {kAVS3DecoderName, C2VendorCodec::VDEC_AVS3},
#endif
#ifdef SUPPORT_VDEC_AVS2
    {kAVS2DecoderName, C2VendorCodec::VDEC_AVS2},
#endif
#ifdef SUPPORT_VDEC_AVS
    {kAVSDecoderName, C2VendorCodec::VDEC_AVS},
#endif
};

}  // namespace android

#endif  // ANDROID_CODEC2_VDA_VIDEO_SUPPORT_H
