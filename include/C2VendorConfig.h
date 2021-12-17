#ifndef C2_VENDOR_CONFIG_H_
#define C2_VENDOR_CONFIG_H_

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>
enum C2AmlParamIndexKind : C2Param::type_index_t {
    kParamIndexStreamPtsUnstable = C2Param::TYPE_INDEX_VENDOR_START,
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


#endif//C2_VENDOR_CONFIG_H_
