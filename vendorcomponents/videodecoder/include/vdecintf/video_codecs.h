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

#ifndef VIDEO_CODECS_H_
#define VIDEO_CODECS_H_

#include <string>

namespace media {

// Video codec profiles. Keep in sync with mojo::VideoCodecProfile (see
// media/mojo/interfaces/media_types.mojom), gpu::VideoCodecProfile (see
// gpu/config/gpu_info.h), and PP_VideoDecoder_Profile (translation is performed
// in content/renderer/pepper/ppb_video_decoder_impl.cc).
// NOTE: These values are histogrammed over time in UMA so the values must never
// ever change (add new values to tools/metrics/histograms/histograms.xml)
// GENERATED_JAVA_ENUM_PACKAGE: org.chromium.media
enum VideoCodecProfile {
  // Keep the values in this enum unique, as they imply format (h.264 vs. VP8,
  // for example), and keep the values for a particular format grouped
  // together for clarity.
  VIDEO_CODEC_PROFILE_UNKNOWN = -1,
  VIDEO_CODEC_PROFILE_MIN = VIDEO_CODEC_PROFILE_UNKNOWN,
  H264PROFILE_MIN = 0,
  H264PROFILE_BASELINE = H264PROFILE_MIN,
  H264PROFILE_MAIN = 1,
  H264PROFILE_EXTENDED = 2,
  H264PROFILE_HIGH = 3,
  H264PROFILE_HIGH10PROFILE = 4,
  H264PROFILE_HIGH422PROFILE = 5,
  H264PROFILE_HIGH444PREDICTIVEPROFILE = 6,
  H264PROFILE_SCALABLEBASELINE = 7,
  H264PROFILE_SCALABLEHIGH = 8,
  H264PROFILE_STEREOHIGH = 9,
  H264PROFILE_MULTIVIEWHIGH = 10,
  H264PROFILE_MAX = H264PROFILE_MULTIVIEWHIGH,
  VP8PROFILE_MIN = 11,
  VP8PROFILE_ANY = VP8PROFILE_MIN,
  VP8PROFILE_MAX = VP8PROFILE_ANY,
  VP9PROFILE_MIN = 12,
  VP9PROFILE_PROFILE0 = VP9PROFILE_MIN,
  VP9PROFILE_PROFILE1 = 13,
  VP9PROFILE_PROFILE2 = 14,
  VP9PROFILE_PROFILE3 = 15,
  VP9PROFILE_MAX = VP9PROFILE_PROFILE3,
  HEVCPROFILE_MIN = 16,
  HEVCPROFILE_MAIN = HEVCPROFILE_MIN,
  HEVCPROFILE_MAIN10 = 17,
  HEVCPROFILE_MAIN_STILL_PICTURE = 18,
  HEVCPROFILE_MAX = HEVCPROFILE_MAIN_STILL_PICTURE,
  DOLBYVISION_MIN = 19,
  DOLBYVISION_PROFILE0 = DOLBYVISION_MIN,
  DOLBYVISION_PROFILE4 = 20,
  DOLBYVISION_PROFILE5 = 21,
  DOLBYVISION_PROFILE7 = 22,
  DOLBYVISION_MAX = DOLBYVISION_PROFILE7,
  MPEG1_PROFILE = 23,
  MPEG2_PROFILE = 24,
  MPEG4_PROFILE = 25,
  MJPEG_PROFILE = 26,
  MPEG2TS_PROFILE = 27,
  VC1_PROFILE = 28,
  AVS_PROFILE = 29,
  AVS2_PROFILE = 30,
  AVS3_PROFILE = 31,
  AV1PROFILE_MIN = 32,
  AV1PROFILE_PROFILE_MAIN = AV1PROFILE_MIN,
  AV1PROFILE_PROFILE_HIGH = 33,
  AV1PROFILE_PROFILE_PRO = 34,
  AV1PROFILE_MAX = AV1PROFILE_PROFILE_PRO,
  VIDEO_CODEC_PROFILE_MAX = AV1PROFILE_PROFILE_PRO,
};

std::string GetProfileName(VideoCodecProfile profile);

}  // namespace media

#endif  // VIDEO_CODECS_H_
