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

#ifndef _C2_Vdec__GRALLOC_WRAPER_H_
#define _C2_Vdec__GRALLOC_WRAPER_H_

#include <C2VdecComponent.h>

// add for compile while there is no gralloc dependency patch
#ifndef AM_GRALLOC_EXT_2
typedef enum {
    GRALLOC_DECODE_PARA_WIDTH,
    GRALLOC_DECODE_PARA_HEIGHT,
    GRALLOC_DECODE_PARA_WALIGN,
    GRALLOC_DECODE_PARA_HALIGN,
    GRALLOC_DECODE_PARA_SIZE,
} AM_GRALLOC_DECODE_PARA_TYPE;
using am_gralloc_decode_para = std::map<AM_GRALLOC_DECODE_PARA_TYPE, uint64_t>;
#endif /*AM_GRALLOC_EXT_H*/

typedef uint32_t (*am_gralloc_get_slot_id_t)();
typedef void (*am_gralloc_free_slot_t)(uint32_t slot_id);
typedef void (*am_gralloc_set_parameters_t)(uint32_t slot_id, am_gralloc_decode_para para_map);
typedef uint64_t (*am_gralloc_compose_slot_id_t)(uint32_t slot_id);

namespace android {

// class wraps the interface of am_gralloc_ext
class GrallocWraper {
public:
    GrallocWraper();
    virtual ~GrallocWraper();

    void setComponent(std::shared_ptr<C2VdecComponent> sharecomp);
    uint64_t getPlatformUsage(C2VdecComponent::DeviceUtil* deviceUtil, const media::Size& size);

    // simple wrapper for gralloc extension api V2
    int32_t getSlotID();
    void freeSlotID();
    void setParameters(C2VdecComponent::DeviceUtil* deviceUtil, const media::Size& size);
    uint64_t getUsageFromSlotId();
    void checkGrallocVersion();
protected:

private:
    // for gralloc extension api V1
    uint64_t getPlatformUsageV1(C2VdecComponent::DeviceUtil* deviceUtil);
    uint64_t getUsageFromDoubleWrite(C2VdecComponent::DeviceUtil* deviceUtil, int32_t doublewrite);
    uint64_t getUsageFromTripleWrite(C2VdecComponent::DeviceUtil* deviceUtil, int32_t triplewrite);

    // for gralloc extension api V2
    uint64_t getPlatformUsageV2(C2VdecComponent::DeviceUtil* deviceUtil, const media::Size& size);
    media::Size calculateRealBufferSize(C2VdecComponent::DeviceUtil* deviceUtil, media::Size size);

    // gralloc extension api v2 interfaces
    am_gralloc_get_slot_id_t am_gralloc_get_slot_id;
    am_gralloc_free_slot_t am_gralloc_free_slot;
    am_gralloc_set_parameters_t am_gralloc_set_parameters;
    am_gralloc_compose_slot_id_t am_gralloc_compose_slot_id;

    int32_t mGrallocVersion;
    int32_t mSlotID;
    uint64_t mUsage;
    std::weak_ptr<C2VdecComponent> mComp;

};

}

#endif