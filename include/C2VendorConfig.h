#ifndef C2_VENDOR_CONFIG_H_
#define C2_VENDOR_CONFIG_H_

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>

enum C2AmlParamIndexKind : C2Param::type_index_t {
    kParamIndexStreamPtsUnstable = C2Param::TYPE_INDEX_VENDOR_START,
    kParamIndexVendorTunerHal = C2Param::TYPE_INDEX_VENDOR_START + 1,
    kParamIndexVendorVdecWorkMode = C2Param:: TYPE_INDEX_VENDOR_START + 2,
    kParamIndexVendorDataSourceType = C2Param:: TYPE_INDEX_VENDOR_START + 3,
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

#endif//C2_VENDOR_CONFIG_H_
