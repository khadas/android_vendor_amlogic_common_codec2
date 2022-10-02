/*
 * Copyright (C) 2022 Amlogic, Inc. All rights reserved.
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

#ifndef C2_VENDOR_AUDIO_CONFIG_H_
#define C2_VENDOR_AUDIO_CONFIG_H_

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>

enum C2AmlParamIndexKind : C2Param::type_index_t {
    kParamIndexVendorAdecCodecId = C2Param:: TYPE_INDEX_VENDOR_START + 0x200,
    kParamIndexVendorExtraDataSize = C2Param:: TYPE_INDEX_VENDOR_START + 0x201,
    kParamIndexVendorExtraData = C2Param:: TYPE_INDEX_VENDOR_START + 0x202,
    kParamIndexVendorBlockAlign = C2Param:: TYPE_INDEX_VENDOR_START + 0x203,
    kParamIndexVendorPassthroughEnable = C2Param:: TYPE_INDEX_VENDOR_START + 0x204,
};


typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorAdecCodecId> C2AdecCodecId;
constexpr char C2_PARAMKEY_VENDOR_ADEC_CODECID[] = "adec.codecid";
constexpr char KEY_VENDOR_CODECID[] = "vendor.adec.codecid.value";

typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorBlockAlign> C2BlockAlign;
constexpr char C2_PARAMKEY_VENDOR_BLOCK_ALIGN[] = "block-align";
constexpr char KEY_VENDOR_BLOCK_ALIGN[] = "vendor.block-align.value";

typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorExtraDataSize> C2ExtraDataSize;
constexpr char C2_PARAMKEY_VENDOR_EXTRA_DATA_SIZE[] = "extra-data-size";
constexpr char KEY_VENDOR_EXTRA_DATA_SIZE[] = "vendor.extra-data-size.value";

typedef C2StreamParam<C2Info, C2BlobValue, kParamIndexVendorExtraData> C2ExtraData;
constexpr char C2_PARAMKEY_VENDOR_EXTRA_DATA[] = "extra-data";
constexpr char KEY_VENDOR_EXTRA_DATA[] = "vendor.extra-data.value";

typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorPassthroughEnable> C2PassthroughEnable;
constexpr char C2_PARAMKEY_VENDOR_PASSTHROUGH_ENABLE[] = "is_passthrough_enable";
constexpr char KEY_VENDOR_PASSTHROUGH_ENABLE[] = "vendor.is_passthrough_enable.value";

#endif//C2_VENDOR_AUDIO_CONFIG_H_
