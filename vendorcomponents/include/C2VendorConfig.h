#ifndef C2_VENDOR_CONFIG_H_
#define C2_VENDOR_CONFIG_H_

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>

enum C2AmlParamIndexKind : C2Param::type_index_t {
    kParamIndexStreamPtsUnstable = C2Param::TYPE_INDEX_VENDOR_START,
    kParamIndexVendorTunerHal,
    kParamIndexVendorVdecWorkMode,
    kParamIndexVendorDataSourceType,
    kParamIndexVendorTunerPassthroughTrickMode,

    /*these are Audio Decoder config parameters.*/
    kParamIndexVendorAdecCodecId = C2Param:: TYPE_INDEX_VENDOR_START + 0x200,
    kParamIndexVendorAdecExtraDataSize,
    kParamIndexVendorAdecExtraData,
    kParamIndexVendorAdecBlockAlign,
    kParamIndexVendorAdecPassthroughEnable,
};

struct C2StreamPtsUnstableStruct {
    inline C2StreamPtsUnstableStruct() = default;
    inline C2StreamPtsUnstableStruct(int32_t val1)
        : enable(val1){}
    int32_t enable;
    DEFINE_AND_DESCRIBE_C2STRUCT(StreamPtsUnstable)
    C2FIELD(enable, "enable")
};

typedef C2StreamParam<C2Setting, C2StreamPtsUnstableStruct, kParamIndexStreamPtsUnstable> C2StreamUnstablePts;
constexpr char C2_PARAMKEY_UNSTABLE_PTS[] = "unstable-pts";//"vendor.unstable-pts.enable"

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
constexpr char KEY_VENDOR_TRAICKID[] = "vendor.tunerhal.passthrough.trick-mode";
constexpr char KEY_VENDOR_TRICKSPEED[] = "vendor.tunerhal.passthrough.trick-speed";
constexpr char KEY_VENDOR_FRAMEADVANCE[] = "vendor.tunerhal.passthrough.frame-advance";

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

constexpr char C2_PARAMKEY_VENDOR_VDEC_WORK_MODE[] = "vdec.workmode";
constexpr char KEY_VENDOR_WORK_MODE[] = "vendor.vdec.workmode.value";
constexpr char C2_PARAMKEY_VENDOR_DATASOURCE_TYPE[] = "datasource.type";
constexpr char KEY_VENDOR_DATASOURCE_TYPE[] = "vendor.datasource.type.value";



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


#endif//C2_VENDOR_CONFIG_H_
