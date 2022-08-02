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
const C2String kVP9DecoderName = "c2.amlogic.vp9.decoder";
const C2String kAV1DecoderName = "c2.amlogic.av1.decoder";
const C2String kMP2VDecoderName = "c2.amlogic.mpeg2.decoder";
const C2String kMP4VDecoderName = "c2.amlogic.mpeg4.decoder";
const C2String kMJPGDecoderName = "c2.amlogic.mjpeg.decoder";

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
const C2String kMp2DecoderName = "c2.amlogic.mp2.decoder";



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
    UNKNOWN = 0xff,
};

struct C2CompomentInputCodec {
    C2String compname;
    InputCodec codec;
};

static C2CompomentInputCodec gC2CompomentInputCodec [] = {
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
    VENC_H264,
    VENC_H265,
    ADEC_MP2,
};

struct C2VendorCompoment {
    std::string compname;
    C2VendorCodec codec;
};

static C2VendorCompoment gC2VideoDecoderCompoments [] = {
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
};


static C2VendorCompoment gC2VideoEncoderCompoments [] = {
    {kH264EncoderName, C2VendorCodec::VENC_H264},
    {kH265EncoderName, C2VendorCodec::VENC_H265},
};

static C2VendorCompoment gC2AudioDecoderCompoments [] = {
    {kMp2DecoderName, C2VendorCodec::ADEC_MP2},
};



/**
 * Returns the C2VDA component store.
 * \retval nullptr if the platform component store could not be obtained
 */
std::shared_ptr<C2ComponentStore> GetCodec2VendorComponentStore();
}  // namespace android

#endif  // ANDROID_CODEC2_VDA_SUPPORT_H
