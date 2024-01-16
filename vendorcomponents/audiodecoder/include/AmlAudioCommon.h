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

#ifndef AML_AUDIO_COMMON_H_
#define AML_AUDIO_COMMON_H_

#include "C2VendorProperty.h"
#include "C2VendorDebug.h"

typedef int8_t int8;
typedef uint8_t uint8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef int32_t int32;
typedef uint32_t uint32;
typedef int64_t int64;
typedef uint64_t uint64;
typedef int32_t Int32;

#if 0//close it, change to C2 unified debug interfaces.
#define C2AUDIO_LOGE ALOGE
#define C2AUDIO_LOGI ALOGI
#define C2AUDIO_LOGW ALOGW
#define C2AUDIO_LOGD ALOGD
#define C2AUDIO_LOGV ALOGV
#define LOG_LINE() ALOGD("[%s:%d]", __FUNCTION__, __LINE__);
#else
#define C2AUDIO_LOGE(fmt, str...) CODEC2_LOG(CODEC2_ADEC_LOG_ERR, fmt, ##str)
#define C2AUDIO_LOGI(fmt, str...) CODEC2_LOG(CODEC2_ADEC_LOG_INFO, fmt, ##str)
#define C2AUDIO_LOGW(fmt, str...) CODEC2_LOG(CODEC2_ADEC_LOG_INFO, fmt, ##str)
#define C2AUDIO_LOGD(fmt, str...) CODEC2_LOG(CODEC2_ADEC_LOG_DEBUG, fmt, ##str)
#define C2AUDIO_LOGV(fmt, str...) CODEC2_LOG(CODEC2_ADEC_LOG_VERBOSE, fmt, ##str)
#endif

#endif//AML_AUDIO_COMMON_H_