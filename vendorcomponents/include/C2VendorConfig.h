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

#ifndef C2_VENDOR_CONFIG_H_
#define C2_VENDOR_CONFIG_H_

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>

enum C2AmlParamIndexKind : C2Param::type_index_t {
    kParamIndexStreamPtsUnstable = C2Param::TYPE_INDEX_VENDOR_START,
    kParamIndexVendorPlayerId,
    kParamIndexVendorTunerHal,
    kParamIndexVendorVdecWorkMode,
    kParamIndexVendorDataSourceType,
    kParamIndexVendorTunerPassthroughTrickMode,
    kParamIndexVendorNetflixVPeek,
    kParamIndexVendorSetAvc4kMMU,
    kParamIndexVendorStreammodeInputdelay,
    kParamIndexVendorErrorPolicy,
    kParamIndexVendorStreammodePipeLinedelay,
    kParamIndexVendorTunerPassthroughEventMask,
    kParamIndexVendorStreammodeHwAvSyncID,
    /*these are Audio Decoder config parameters.*/
    kParamIndexVendorAdecCodecId = C2Param:: TYPE_INDEX_VENDOR_START + 0x200,
    kParamIndexVendorAdecExtraDataSize,
    kParamIndexVendorAdecExtraData,
    kParamIndexVendorAdecBlockAlign,
    kParamIndexVendorAdecPassthroughEnable,

    /*these are Video Encoder config parameters.*/
    kParamIndexVendorVencCanvasMode = C2Param:: TYPE_INDEX_VENDOR_START + 0x400,
};

struct C2StreamPtsUnstableStruct {
    inline C2StreamPtsUnstableStruct() = default;
    inline C2StreamPtsUnstableStruct(int32_t val1)
        : enable(val1){}
    int32_t enable;
    DEFINE_AND_DESCRIBE_C2STRUCT(StreamPtsUnstable)
    C2FIELD(enable, "enable")
};

/* ================================ video decoder Config Parameter ================================ */
typedef C2StreamParam<C2Setting, C2StreamPtsUnstableStruct, kParamIndexStreamPtsUnstable> C2StreamUnstablePts;
constexpr char C2_PARAMKEY_UNSTABLE_PTS[] = "unstable-pts";//"vendor.unstable-pts.enable"

typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorPlayerId> C2VendorPlayerId;
constexpr char C2_PARAMKEY_PLAYER_ID[] = "player-id";//"vendor.player-id.value"
constexpr char KEY_VENDOR_PLAYER_ID[] = "vendor.player-id.value";

struct C2VendorTunerHalStruct {
    inline C2VendorTunerHalStruct() = default;
    inline C2VendorTunerHalStruct(int32_t filterid, int32_t syncid)
        : videoFilterId(filterid), hwAVSyncId(syncid) {}

    int32_t videoFilterId;
    int32_t hwAVSyncId;

    DEFINE_AND_DESCRIBE_C2STRUCT(VendorTunerHal)
    C2FIELD(videoFilterId, "video-filter-id")
    C2FIELD(hwAVSyncId, "hw-av-sync-id")

};

typedef C2PortParam<C2Setting, C2VendorTunerHalStruct, kParamIndexVendorTunerHal> C2VendorTunerHalParam;
constexpr char C2_PARAMKEY_VENDOR_TunerHal[] = "tunerhal";
constexpr char KEY_VENDOR_FILTERID[] = "vendor.tunerhal.video-filter-id";
constexpr char KEY_VENDOR_TunerHal_SYNCID[] = "vendor.tunerhal.hw-av-sync-id";

struct C2VendorTunerPassthroughTrickModeStruct {
    inline C2VendorTunerPassthroughTrickModeStruct() = default;
    inline C2VendorTunerPassthroughTrickModeStruct(int32_t trick_mode, int32_t trick_speed, int32_t frame_advance)
       :trickMode(trick_mode), trickSpeed(trick_speed), frameAdvance(frame_advance) {}
    int32_t trickMode;
    int32_t trickSpeed;
    int32_t frameAdvance;
    DEFINE_AND_DESCRIBE_C2STRUCT(VendorTunerPassthroughTrickMode)
    C2FIELD(trickMode, "trick-mode")
    C2FIELD(trickSpeed, "trick-speed")
    C2FIELD(frameAdvance, "frame-advance")
};

typedef C2PortParam<C2Setting, C2VendorTunerPassthroughTrickModeStruct, kParamIndexVendorTunerPassthroughTrickMode> C2VendorTunerPassthroughTrickMode;
constexpr char C2_PARAMKEY_VENDOR_TunerPassthroughTrickMode[] = "tunerhal.passthrough";
constexpr char KEY_VENDOR_TRAICKMODE[] = "vendor.tunerhal.passthrough.trick-mode";
constexpr char KEY_VENDOR_TRICKSPEED[] = "vendor.tunerhal.passthrough.trick-speed";
constexpr char KEY_VENDOR_FRAMEADVANCE[] = "vendor.tunerhal.passthrough.frame-advance";

struct C2VendorTunerPassthroughEventMaskStruct {
    inline C2VendorTunerPassthroughEventMaskStruct() = default;
    inline C2VendorTunerPassthroughEventMaskStruct(int64_t event_mask)
        :eventMask(event_mask) {}
    int64_t eventMask;
    DEFINE_AND_DESCRIBE_C2STRUCT(VendorTunerPassthroughEventMask)
    C2FIELD(eventMask, "event-mask")
};

typedef C2PortParam<C2Setting, C2VendorTunerPassthroughEventMaskStruct, kParamIndexVendorTunerPassthroughEventMask> C2VendorTunerPassthroughEventMask;
constexpr char C2_PARAMKEY_VENDOR_TunerPassthroughEventMask[] = "tunerhal.passthrough";
constexpr char KEY_VENDOR_EVENTMASK[] = "vendor.tunerhal.passthrough.event-mask";

enum VDEC_WORKMODE {
    VDEC_FRAMEMODE,
    VDEC_STREAMMODE,
};

enum DATASOURCE_TYPE {
    DATASOURCE_DEFAULT,
    DATASOURCE_DMX,
};

typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorVdecWorkMode> C2VdecWorkMode;
typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorDataSourceType> C2DataSourceType;
typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorStreammodeInputdelay> C2StreamModeInputDelay;
typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorStreammodePipeLinedelay> C2StreamModePipeLineDelay;
typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorStreammodeHwAvSyncID> C2StreamModeHwAvSyncId;


constexpr char C2_PARAMKEY_VENDOR_VDEC_WORK_MODE[] = "vdec.workmode";
constexpr char KEY_VENDOR_WORK_MODE[] = "vendor.vdec.workmode.value";
constexpr char C2_PARAMKEY_VENDOR_DATASOURCE_TYPE[] = "datasource.type";
constexpr char KEY_VENDOR_DATASOURCE_TYPE[] = "vendor.datasource.type.value";
constexpr char C2_PARAMKEY_VENDOR_STREAMMODE_INPUT_DELAY[] = "streammode.inputdelay";
constexpr char KEY_VENDOR_STREAMMODE_INPUT_DELAY[] = "vendor.streammode.inputdelay.value";
constexpr char C2_PARAMKEY_VENDOR_STREAMMODE_PIPELINE_DELAY[] = "streammode.pipelinedelay";
constexpr char KEY_VENDOR_STREAMMODE_PIPELINE_DELAY[] = "vendor.streammode.pipelinedelay.value";
constexpr char C2_PARAMKEY_VENDOR_STREAMMODE_HWAVSYNCID[] = "streammode.hwavsyncid";
constexpr char KEY_VENDOR_STREAMMODE_HWAVSYNCID[] = "vendor.streammode.hwavsyncid.value";

struct C2VendorNetflixVPeekStruct {
    inline C2VendorNetflixVPeekStruct() = default;
    inline C2VendorNetflixVPeekStruct(int32_t val)
        :vpeek(val) {}
    int32_t vpeek;
    DEFINE_AND_DESCRIBE_C2STRUCT(VendorNetflixVPeek)
    C2FIELD(vpeek, "video-peek-in-tunnel")
};

typedef C2PortParam<C2Setting, C2VendorNetflixVPeekStruct, kParamIndexVendorNetflixVPeek> C2VendorNetflixVPeek;
constexpr char C2_PARAMKEY_VENDOR_NETFLIXVPEEK[] = "nrdp";
constexpr char KEY_VENDOR_NETFLIXVPEEK[] = "vendor.nrdp.video-peek-in-tunnel";

typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorSetAvc4kMMU> C2Avc4kMMU;
constexpr char C2_PARAMKEY_VENDOR_AVC_4K_MMU[] = "vdec.avc-4k-mmu";
constexpr char KEY_VENDOR_AVC_4K_MMU[] = "vendor.vdec.avc-4k-mmu.value";

typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorErrorPolicy> C2ErrorPolicy;
constexpr char C2_PARAMKEY_VENDOR_ERROR_POLICY[] = "vdec.error-policy";
constexpr char KEY_VENDOR_ERROR_POLICY[] = "vendor.vdec.error-policy.value";

/* ================================ Audio Config Parameter ================================ */
typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorAdecCodecId> C2SdkCodecId;
constexpr char C2_PARAMKEY_VENDOR_CODECID[] = "adec.codec-id";
constexpr char KEY_VENDOR_CODECID[] = "vendor.adec.codec-id.value";

typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorAdecBlockAlign> C2BlockAlign;
constexpr char C2_PARAMKEY_VENDOR_BLOCK_ALIGN[] = "adec.block-align";
constexpr char KEY_VENDOR_BLOCK_ALIGN[] = "vendor.adec.block-align.value";

typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorAdecExtraDataSize> C2ExtraDataSize;
constexpr char C2_PARAMKEY_VENDOR_EXTRA_DATA_SIZE[] = "adec.extra-data-size";
constexpr char KEY_VENDOR_EXTRA_DATA_SIZE[] = "vendor.adec.extra-data-size.value";

typedef C2StreamParam<C2Info, C2BlobValue, kParamIndexVendorAdecExtraData> C2ExtraData;
constexpr char C2_PARAMKEY_VENDOR_EXTRA_DATA[] = "adec.extra-data";
constexpr char KEY_VENDOR_EXTRA_DATA[] = "vendor.adec.extra-data.value";

typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorAdecPassthroughEnable> C2PassthroughEnable;
constexpr char C2_PARAMKEY_VENDOR_PASSTHROUGH_ENABLE[] = "adec.is_passthrough_enable";
constexpr char KEY_VENDOR_PASSTHROUGH_ENABLE[] = "vendor.adec.is_passthrough_enable.value";


/* ================================ Video Encoder Config Parameter ================================ */
typedef C2PortParam<C2Setting, C2Int32Value, kParamIndexVendorVencCanvasMode> C2VencCanvasMode;
constexpr char C2_PARAMKEY_VENDOR_VENC_CANVAS_MODE[] = "venc.canvasmode";
constexpr char KEY_VENDOR_CANVAS_MODE[] = "vendor.venc.canvasmode.value";



#endif//C2_VENDOR_CONFIG_H_
