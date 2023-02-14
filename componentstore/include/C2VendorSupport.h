// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_CODEC2_VDA_SUPPORT_H
#define ANDROID_CODEC2_VDA_SUPPORT_H

#include <C2Component.h>

#include <memory>

namespace android {

const C2String kH264DecoderName = "c2.amlogic.avc.decoder";
const C2String kH265DecoderName = "c2.amlogic.hevc.decoder";
const C2String kVP9DecoderName  = "c2.amlogic.vp9.decoder";
const C2String kAV1DecoderName  = "c2.amlogic.av1.decoder";
const C2String kMP2VDecoderName = "c2.amlogic.mpeg2.decoder";
const C2String kMP4VDecoderName = "c2.amlogic.mpeg4.decoder";
const C2String kMJPGDecoderName = "c2.amlogic.mjpeg.decoder";
const C2String kAVSDecoderName  = "c2.amlogic.avs.decoder";
const C2String kAVS2DecoderName = "c2.amlogic.avs2.decoder";

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

/*encoder*/
const C2String kH264EncoderName = "c2.amlogic.avc.encoder";
const C2String kH265EncoderName = "c2.amlogic.hevc.encoder";

/*audio*/
const C2String kMP2DecoderName      = "c2.amlogic.audio.decoder.mp2";
const C2String kAACDecoderName      = "c2.amlogic.audio.decoder.aac";
const C2String kAC3DecoderName      = "c2.amlogic.audio.decoder.ac3";
const C2String kEC3DecoderName      = "c2.amlogic.audio.decoder.eac3";
const C2String kFFMPEGDecoderName   = "c2.amlogic.audio.decoder.ffmpeg";
const C2String kDTSDecoderName      = "c2.amlogic.audio.decoder.dts";
const C2String kDTSHDDecoderName    = "c2.amlogic.audio.decoder.dtshd";
const C2String kDTSSEDecoderName    = "c2.amlogic.audio.decoder.dtse";
const C2String kAC4DecoderName      = "c2.amlogic.audio.decoder.ac4";


enum class InputCodec {
    H264,
    H265,
    VP9,
    AV1,
    DVHE,
    DVAV,
    DVAV1,
    MP2V,
    MP4V,
    MJPG,
    AVS2,
    AVS,
    UNKNOWN = 0xff,
};

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
    {kMP4VDecoderName, InputCodec::MP4V},
    {kMJPGDecoderName, InputCodec::MJPG},
    {kAVS2DecoderName, InputCodec::AVS2},
    {kAVSDecoderName, InputCodec::AVS},
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
#ifdef SUPPORT_VDEC_AVS2
    VDEC_AVS2,
#endif
#ifdef SUPPORT_VDEC_AVS
    VDEC_AVS,
#endif
#ifdef SUPPORT_VDEC_AVS
    VDEC_TYPE_MAX = VDEC_AVS,
#elif  SUPPORT_VDEC_AVS2
    VDEC_TYPE_MAX = VDEC_AV2,
#else
    VDEC_TYPE_MAX = VDEC_MJPG,
#endif
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
    ADEC_MP2,
    ADEC_AAC,
    ADEC_AC3,
    ADEC_EAC3,
    ADEC_FFMPEG,
    ADEC_DTS,
    ADEC_DTSHD,
    ADEC_DTSE,
    ADEC_AC4,
};

struct C2VendorComponent {
    std::string compname;
    C2VendorCodec codec;
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
    {kMP4VDecoderName, C2VendorCodec::VDEC_MP4V},
    {kMJPGDecoderName, C2VendorCodec::VDEC_MJPG},
#ifdef SUPPORT_VDEC_AVS2
    {kAVS2DecoderName, C2VendorCodec::VDEC_AVS2},
#endif
#ifdef SUPPORT_VDEC_AVS
    {kAVSDecoderName, C2VendorCodec::VDEC_AVS},
#endif
};

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

static C2VendorComponent gC2VideoEncoderComponents [] = {
    {kH264EncoderName, C2VendorCodec::VENC_H264},
    {kH265EncoderName, C2VendorCodec::VENC_H265},
};

static C2VendorComponent gC2AudioDecoderComponents [] = {
    {kMP2DecoderName, C2VendorCodec::ADEC_MP2},
    {kAACDecoderName, C2VendorCodec::ADEC_AAC},
    {kAC3DecoderName, C2VendorCodec::ADEC_AC3},
    {kEC3DecoderName, C2VendorCodec::ADEC_EAC3},
    {kFFMPEGDecoderName, C2VendorCodec::ADEC_FFMPEG},
    {kDTSDecoderName, C2VendorCodec::ADEC_DTS},
    {kDTSHDDecoderName, C2VendorCodec::ADEC_DTSHD},
    {kDTSSEDecoderName, C2VendorCodec::ADEC_DTSE},
    {kAC4DecoderName, C2VendorCodec::ADEC_AC4},
};



/**
 * Returns the C2VDA component store.
 * \retval nullptr if the platform component store could not be obtained
 */
std::shared_ptr<C2ComponentStore> GetCodec2VendorComponentStore();
}  // namespace android

#endif  // ANDROID_CODEC2_VDA_SUPPORT_H
