/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
#ifndef ANDROID_CODEC2_VDA_AUDIO_SUPPORT_H
#define ANDROID_CODEC2_VDA_AUDIO_SUPPORT_H

#include <memory>
#include <C2Component.h>
#include <C2VendorSupport.h>

namespace android {

/*audio*/
const C2String kMP2DecoderName      = "c2.amlogic.audio.decoder.mp2";
const C2String kAACDecoderName      = "c2.amlogic.audio.decoder.aac";
const C2String kAC3DecoderName      = "c2.amlogic.audio.decoder.ac3";
const C2String kEC3DecoderName      = "c2.amlogic.audio.decoder.eac3";
const C2String kFFMPEGDecoderName   = "c2.amlogic.audio.decoder.ffmpeg";
const C2String kDTSDecoderName      = "c2.amlogic.audio.decoder.dts";
const C2String kDTSHDDecoderName    = "c2.amlogic.audio.decoder.dtshd";
const C2String kDTSEDecoderName     = "c2.amlogic.audio.decoder.dtse";
const C2String kDTSUHDDecoderName   = "c2.amlogic.audio.decoder.dtsuhd";
const C2String kAC4DecoderName      = "c2.amlogic.audio.decoder.ac4";

static C2VendorComponent gC2AudioDecoderComponents [] = {
    {kMP2DecoderName, C2VendorCodec::ADEC_MP2},
    {kAACDecoderName, C2VendorCodec::ADEC_AAC},
    {kAC3DecoderName, C2VendorCodec::ADEC_AC3},
    {kEC3DecoderName, C2VendorCodec::ADEC_EAC3},
    {kFFMPEGDecoderName, C2VendorCodec::ADEC_FFMPEG},
    {kDTSDecoderName, C2VendorCodec::ADEC_DTS},
    {kDTSHDDecoderName, C2VendorCodec::ADEC_DTSHD},
    {kDTSEDecoderName, C2VendorCodec::ADEC_DTSE},
    {kDTSUHDDecoderName, C2VendorCodec::ADEC_DTSUHD},
    {kAC4DecoderName, C2VendorCodec::ADEC_AC4},
};

}  // namespace android

#endif  // ANDROID_CODEC2_VDA_AUDIO_SUPPORT_H

