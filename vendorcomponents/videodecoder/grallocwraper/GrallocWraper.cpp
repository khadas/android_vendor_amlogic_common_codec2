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

#define LOG_NDEBUG 0
#define LOG_TAG "C2VdecGrallocWraper"

#include <am_gralloc_ext.h>
#include <hardware/gralloc1.h>
#include <C2VendorDebug.h>
#include <C2VdecDeviceUtil.h>
#include <C2VendorProperty.h>
#include <dlfcn.h>
#include "GrallocWraper.h"

#define C2VdecGW_LOG(level, fmt, str...) \
    if (comp != nullptr) { \
        CODEC2_LOG(level, "[%d#%d %s@%d]"#fmt, comp->mSessionID, comp->mDecoderID, __func__, __LINE__, ##str); \
    } else { \
        CODEC2_LOG(level, "[%s@%d]"#fmt,  __func__, __LINE__, ##str); \
    }

#define CODEC_ALIGN(value, base) (((value) + ((base)-1)) & ~((base)-1))
#define DEFAULT_HEIGHT_ALIGN_SIZE 64
#define DEFAULT_WIDTH_ALIGN_SIZE 64
#define BUFFER_SIZE_576P_WIDTH 1024
#define BUFFER_SIZE_576P_HEIGHT 576
#define BUFFER_SIZE_1080P_WIDTH 2048
#define BUFFER_SIZE_1080P_HEIGHT 1088

#ifdef LockWeakPtrWithReturnVal
#undef LockWeakPtrWithReturnVal
#endif
#define LockWeakPtrWithReturnVal(name, weak, retval) \
    auto name = weak.lock(); \
    if (name == nullptr) { \
        C2VdecGW_LOG(CODEC2_LOG_ERR, "[%s:%d] null ptr, please check", __func__, __LINE__); \
        return retval;\
    }

#ifdef LockWeakPtrWithReturnVoid
#undef LockWeakPtrWithReturnVoid
#endif
#define LockWeakPtrWithReturnVoid(name, weak) \
    auto name = weak.lock(); \
    if (name == nullptr) { \
        C2VdecGW_LOG(CODEC2_LOG_ERR, "[%s:%d] null ptr, please check", __func__, __LINE__); \
        return;\
    }

namespace android {
static void* gGrallocHandle = NULL;


GrallocWraper::GrallocWraper() {
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s@%d] enter", __func__, __LINE__);
    mSlotID = -1;
    mUsage = 0;
    mGrallocVersion = 1;
    am_gralloc_get_slot_id = NULL;
    am_gralloc_free_slot = NULL;
    am_gralloc_set_parameters = NULL;
    am_gralloc_compose_slot_id = NULL;

    checkGrallocVersion();
}

GrallocWraper::~GrallocWraper() {
    CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s@%d] enter", __func__, __LINE__);
    freeSlotID();
}

 void GrallocWraper::setComponent(std::shared_ptr<C2VdecComponent> sharecomp) {
    mComp = sharecomp;
 }

uint64_t GrallocWraper::getPlatformUsage(C2VdecComponent::DeviceUtil* deviceUtil, const media::Size& size) {
    uint64_t usage = 0;
    LockWeakPtrWithReturnVal(comp, mComp, 0);
    bool supportV2 = property_get_bool(C2_PROPERTY_VDEC_SUPPORT_GRALLOC_V2, true);
    if (mGrallocVersion == 1 || !supportV2) {
        usage = getPlatformUsageV1(deviceUtil);
    } else if (mGrallocVersion == 2) {
        usage = getPlatformUsageV2(deviceUtil, size);
    } else {
        CODEC2_LOG(CODEC2_LOG_ERR, "gralloc version not supported");
    }

    C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "Got usage:%llx, supportV2:%d", (unsigned long long)usage, supportV2);
    mUsage = usage;
    return usage;
}

uint64_t GrallocWraper::getPlatformUsageV1(C2VdecComponent::DeviceUtil* deviceUtil) {
    uint64_t usage = 0;
    LockWeakPtrWithReturnVal(comp, mComp, usage);

#ifdef SUPPORT_GRALLOC_REPLACE_BUFFER_USAGE
    if (deviceUtil->needDecoderReplaceBufferForDiPost()) {
        usage =  am_gralloc_get_video_decoder_replace_buffer_usage();
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "using am_gralloc_get_video_decoder_replace_buffer_usage:%llx", (unsigned long long)usage);
        return usage & C2MemoryUsage::PLATFORM_MASK;
    }
#endif
    C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "do not support am_gralloc_get_video_decoder_replace_buffer_usage");

    int32_t doubleWrite = deviceUtil->getDoubleWriteModeValue();
    int32_t tripleWrite = deviceUtil->getTripleWriteModeValue();
    auto getUsage = [=] (int32_t doubleWrite, int32_t tripleWrite) -> uint64_t {
        uint64_t ret = 0;
        int32_t defaultDw = deviceUtil->getPropertyDoubleWrite();
        int32_t doubleWriteValue = (defaultDw >= 0) ? defaultDw : doubleWrite;

        int32_t defaultTw = deviceUtil->getPropertyTripleWrite();
        int32_t tripleWriteValue = (defaultTw >= 0) ? defaultTw : tripleWrite;

        if (doubleWriteValue == 0 && tripleWriteValue != 0)
            ret = getUsageFromTripleWrite(deviceUtil, tripleWriteValue);
        else
            ret = getUsageFromDoubleWrite(deviceUtil, doubleWriteValue);

        return ret;
    };

    if (deviceUtil->mIsYcbRP010Stream) {
        if (deviceUtil->mHwSupportP010) { // hardware support 10bit, use triple write.
            usage = getUsage(doubleWrite, tripleWrite);
            C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] doubleWrite:0x%llx triplewrite:0x%llx usage:%llx",__func__, __LINE__,
                            (unsigned long long)doubleWrite, (unsigned long long)tripleWrite, (unsigned long long)usage);
        } else { // soft support 10 bit, use double write.
            int32_t doubleWrite = deviceUtil->getDoubleWriteModeValue();
            usage = getUsageFromDoubleWrite(deviceUtil, doubleWrite);
            if (deviceUtil->mUseSurfaceTexture || deviceUtil->mNoSurface) {
                // surfacetext or no surface alloc real 10bit buf
                // surface mode need alloc small buf, so we need not set 'GRALLOC1_PRODUCER_USAGE_PRIVATE_3' usage,
                // because if we set GRALLOC1_PRODUCER_USAGE_PRIVATE_3 usage value,the dma buf we alloced is real
                // 10bit buf with the setting w h value.
                usage = am_gralloc_get_video_decoder_OSD_buffer_usage();
                usage = usage | GRALLOC1_PRODUCER_USAGE_PRIVATE_3;
            }
            C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] doublewrite:0x%llx usage:%llx",__func__, __LINE__, (unsigned long long)doubleWrite, (unsigned long long)usage);
        }
    } else if (deviceUtil->mUseSurfaceTexture || deviceUtil->mNoSurface) {
        usage = am_gralloc_get_video_decoder_OSD_buffer_usage();
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] OSD usage:%llx",__func__, __LINE__, (unsigned long long)usage);
    } else {
        int32_t doubleWrite = deviceUtil->getDoubleWriteModeValue();
        if (deviceUtil->mForceFullUsage) {
            usage = am_gralloc_get_video_decoder_full_buffer_usage();
            C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s:%d] Force use full usage:%llx",__func__, __LINE__, (unsigned long long)usage);
        } else {
            usage = getUsage(doubleWrite, tripleWrite);
            C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "get usage:%llx dw:%d tw:%d", (unsigned long long)usage, doubleWrite, tripleWrite);
        }
    }

    return usage;
}

uint64_t GrallocWraper::getPlatformUsageV2(C2VdecComponent::DeviceUtil* deviceUtil, const media::Size& size) {
    LockWeakPtrWithReturnVal(comp, mComp, 0);
    C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "enter, mSlotID:%d", mSlotID);
    if (mSlotID != -1) {
        return mUsage;
    }
    if (getSlotID() == -1) {
        // get slot id failed, usage Gralloc Extension api V1
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "mSlotID:%d is invalid, fallback to Gralloc Extension API V1", mSlotID);
        return getPlatformUsageV1(deviceUtil);
    }

    media::Size realSize = calculateRealBufferSize(deviceUtil, size);
    setParameters(deviceUtil, realSize);

    uint64_t usage = getUsageFromSlotId();
    if (deviceUtil->mUseSurfaceTexture || deviceUtil->mNoSurface) {
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "UseSurfaceTexture or Buffer mode, usage[%llx] add OSD_buffer usage bit[%llx]",
                (unsigned long long)usage, (unsigned long long)am_gralloc_get_omx_osd_producer_usage());
        usage |= am_gralloc_get_omx_osd_producer_usage();
    }
    if (deviceUtil->mIsYcbRP010Stream && !deviceUtil->mHwSupportP010) {
        if (deviceUtil->mUseSurfaceTexture || deviceUtil->mNoSurface) {
            // surfacetext or no surface alloc real 10bit buf
            // surface mode need alloc small buf, so we need not set 'GRALLOC1_PRODUCER_USAGE_PRIVATE_3' usage,
            // because if we set GRALLOC1_PRODUCER_USAGE_PRIVATE_3 usage value,the dma buf we alloced is real
            // 10bit buf with the setting w h value.
            C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "P010 soft decode, SurfaceTexture or Buffer mode, add PRIVATE_3 usage");
            usage |= GRALLOC1_PRODUCER_USAGE_PRIVATE_3;
        }
    }

    return usage | GRALLOC1_PRODUCER_USAGE_VIDEO_DECODER;
}

media::Size GrallocWraper::calculateRealBufferSize(C2VdecComponent::DeviceUtil* deviceUtil, media::Size ori) {
    LockWeakPtrWithReturnVal(comp, mComp, ori);
    media::Size result, size;

#ifdef SUPPORT_GRALLOC_REPLACE_BUFFER_USAGE
    if (deviceUtil->needDecoderReplaceBufferForDiPost()) {
        // decoder replace buffer mode, only need a little buffer as a stub
        result.set_width(16);
        result.set_height(16);
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "decoder replace buffer mode");
        return result;
    }
#endif

    // use max size for some case
    size = ori;
    if (deviceUtil->needAllocWithMaxSize()) {
        uint32_t maxWidth = 0;
        uint32_t maxHeight = 0;
        deviceUtil->getMaxBufWidthAndHeight(maxWidth, maxHeight);
        size.set_width(maxWidth);
        size.set_height(maxHeight);
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "neede alloc with max size");
    }

    int32_t doubleWrite = deviceUtil->getDoubleWriteModeValue();
    int32_t tripleWrite = deviceUtil->getTripleWriteModeValue();
    C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL2, "double write: 0x%x, triple write: 0x%x", doubleWrite, tripleWrite);

    if (doubleWrite == 0 && tripleWrite != 0) {
        doubleWrite = tripleWrite;
    }

    int32_t widthAlign = deviceUtil->getDecoderWidthAlign();
    if (widthAlign == -1) {
        widthAlign = DEFAULT_WIDTH_ALIGN_SIZE;
    }

    // surface texture or buffer mode need alloc real size buffer
    if (deviceUtil->mUseSurfaceTexture || deviceUtil->mNoSurface) {
        result.set_width(CODEC_ALIGN(size.width(), widthAlign));
        result.set_height(CODEC_ALIGN(size.height(), DEFAULT_HEIGHT_ALIGN_SIZE));
        // software decode p010 stream, we use PRIVATE_3 usage which mean height align is **1**
        if (deviceUtil->mIsYcbRP010Stream && !deviceUtil->mHwSupportP010) {
            result.set_height(CODEC_ALIGN(result.height(), 1));
            C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "software decode p010 stream, use height align is **1**");
        }
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "surface texture or buffer mode, use real buffer size after aligned");
        return result;
    }

    // calculate result size for each double write
    switch (doubleWrite) {
    case 0:
    {
        // only afbc, dose not need uvm buffers, only alloc a small buffer for stub
        result.set_width(16);
        result.set_height(16);
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL2, "double write: 0x%x, only need small buffer for stub", doubleWrite);
        break;
    }
    case 1:
    case 0x10001:
    {
        // afbc + nv21(1:1)
        result.set_width(CODEC_ALIGN(size.width(), widthAlign));
        result.set_height(CODEC_ALIGN(size.height(), DEFAULT_HEIGHT_ALIGN_SIZE));
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL2, "double write: %x, need 1:1 buffer", doubleWrite);
        break;
    }
    case 2:
    case 0x10002:
    case 3:
    case 0x10003:
    {
        // afbc + nv21(1:16)
        result.set_width(CODEC_ALIGN(size.width() / 4, widthAlign));
        result.set_height(CODEC_ALIGN(size.height() / 4, DEFAULT_HEIGHT_ALIGN_SIZE));
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL2, "double write: 0x%x, need 1:16 buffer", doubleWrite);
        break;
    }
    case 4:
    case 0x10004:
    case 5:
    case 0x10005:
    {
        // afbc + nv21(1:4)
        result.set_width(CODEC_ALIGN(size.width() / 2, widthAlign));
        result.set_height(CODEC_ALIGN(size.height() / 2, DEFAULT_HEIGHT_ALIGN_SIZE));
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL2, "double write: 0x%x, need 1:4 buffer", doubleWrite);
        break;
    }
    case 8:
    case 0x10008:
    {
        // afbc + nv21(1:64)
        result.set_width(CODEC_ALIGN(size.width() / 8, widthAlign));
        result.set_height(CODEC_ALIGN(size.height() / 8, DEFAULT_HEIGHT_ALIGN_SIZE));
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL2, "double write: 0x%x, need 1:64 buffer", doubleWrite);
        break;
    }
    case 0x10:
    case 0x10010:
    {
        // nv21(1:1)
        result.set_width(CODEC_ALIGN(size.width(), widthAlign));
        result.set_height(CODEC_ALIGN(size.height(), DEFAULT_HEIGHT_ALIGN_SIZE));
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL2, "double write: 0x%x, need 1:1 buffer", doubleWrite);
        break;
    }
    case 0x100:
    case 0x10100:
    {
        // 1080P self-adaption: <= 1080P(1:1); > 1080P(dw4 1:4)
        result.set_width(BUFFER_SIZE_1080P_WIDTH);
        result.set_height(BUFFER_SIZE_1080P_HEIGHT);
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL2, "double write: 0x%x use 1080p buffer", doubleWrite);
        break;
    }
    case 0x200:
    case 0x10200:
    {
        // 1080P self-adaption: <= 1080p(1:1); > 1080P(dw2 1:16)
        // use 2048x1088 buffer size to avoid reallocat buffers
        result.set_width(BUFFER_SIZE_1080P_WIDTH);
        result.set_height(BUFFER_SIZE_1080P_HEIGHT);
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL2, "double write: 0x%x use 1080p buffer", doubleWrite);
        break;
    }
    case 0x300:
    case 0x10300:
    {
        // 576P self-adaption: <= 576p(1:1); > 576P(dw4 1:4)
        // use 1024x576 buffer size to avoid reallocat buffers
        result.set_width(BUFFER_SIZE_576P_WIDTH);
        result.set_height(BUFFER_SIZE_576P_HEIGHT);
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL2, "double write: 0x%x use 576p buffer", doubleWrite);
        break;
    }
    case 0x400:
    case 0x10400:
    {
        // more-step-adaption: <= 576p(1:1); <= 1080P(1:4); <= 4k(1:16); > 4k(1:64)
        // use 1024x576 buffer size to avoid reallocat buffers
        result.set_width(BUFFER_SIZE_576P_WIDTH);
        result.set_height(BUFFER_SIZE_576P_HEIGHT);
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL2, "double write: 0x%x use 576p buffer", doubleWrite);
        break;
    }
    default:
    {
        // not supported double write, use 1:1 buffer for default
        result.set_width(CODEC_ALIGN(size.width(), widthAlign));
        result.set_height(CODEC_ALIGN(size.height(), DEFAULT_HEIGHT_ALIGN_SIZE));
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL2, "unknown double write: 0x%x, use 1:1 buffer", doubleWrite);
        break;
    }
    }

    // for all interlaced stream, using half of buffer
    if (deviceUtil->isNeedHalfHeightBuffer()) {
        C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL2, "h265 interlaced stream, the height only need a half");
        result.set_height(CODEC_ALIGN(result.height()/2, DEFAULT_HEIGHT_ALIGN_SIZE));
    }

    C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "size[%dx%d => %dx%d], dw:0x%x, tw:0x%x, widthAlign:%d, interlaced:%d",
            ori.width(), ori.height(), result.width(), result.height(), doubleWrite, tripleWrite, widthAlign, deviceUtil->isInterlaced());
    return result;
}

uint64_t GrallocWraper::getUsageFromDoubleWrite(C2VdecComponent::DeviceUtil* deviceUtil, int32_t doubleWrite) {
    uint64_t usage = 0;
    LockWeakPtrWithReturnVal(comp, mComp, 0);
    switch (doubleWrite) {
        case 0:
            usage = am_gralloc_get_video_decoder_full_buffer_usage();
            break;
        case 1:
        case 0x10:
            usage = am_gralloc_get_video_decoder_full_buffer_usage();
            break;
        case 2:
        case 3:
        case 0x200:
        case 0x400:
            usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
            break;
        case 4:
        case 0x100:
        case 0x300:
            usage = am_gralloc_get_video_decoder_quarter_buffer_usage();
            break;
        case 0x10001:
            usage = am_gralloc_get_video_decoder_full_buffer_usage();
            break;
        case 0x10003:
            usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
            break;
        case 0x10004:
            usage = am_gralloc_get_video_decoder_quarter_buffer_usage();
            break;
        case 0x10008:
            usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
            break;
        case 0x10200:
            usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
            break;
        default:
            usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
            break;
    }

    C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "doubleWrite:0x%d usage:0x%llx",doubleWrite, (unsigned long long)usage);
    return usage;
}

uint64_t GrallocWraper::getUsageFromTripleWrite(C2VdecComponent::DeviceUtil* deviceUtil, int32_t tripleWrite) {
    uint64_t usage = 0;
    LockWeakPtrWithReturnVal(comp, mComp, 0);
    switch (tripleWrite) {
        case 0:
            usage = am_gralloc_get_video_decoder_full_buffer_usage();
            break;
        case 1:
            usage = am_gralloc_get_video_decoder_full_buffer_usage();
            break;
        case 3:
            usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
            break;
        case 4:
            usage = am_gralloc_get_video_decoder_quarter_buffer_usage();
            break;
        case 0x10001:
            usage = am_gralloc_get_video_decoder_full_buffer_usage();
            break;
        case 0x10003:
            usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
            break;
        case 0x10004:
            usage = am_gralloc_get_video_decoder_quarter_buffer_usage();
            break;
        case 0x10008:
            usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
            break;
        case 0x10200:
            usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
            break;
        default:
            usage = am_gralloc_get_video_decoder_one_sixteenth_buffer_usage();
        break;
    }

    C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "triplewrite:0x%d usage:0x%llx", tripleWrite, (unsigned long long)usage);
    return usage;
}

void GrallocWraper::checkGrallocVersion() {
    if (gGrallocHandle == NULL) {
        gGrallocHandle = dlopen("libamgralloc_ext.so", RTLD_NOW);
    }

    if (gGrallocHandle != NULL) {
        am_gralloc_get_slot_id = (am_gralloc_get_slot_id_t)dlsym(gGrallocHandle, "_Z22am_gralloc_get_slot_idv");
        am_gralloc_free_slot = (am_gralloc_free_slot_t)dlsym(gGrallocHandle, "_Z20am_gralloc_free_slotj");
        am_gralloc_set_parameters = (am_gralloc_set_parameters_t)dlsym(gGrallocHandle,
            "_Z25am_gralloc_set_parametersjNSt3__13mapI27AM_GRALLOC_DECODE_PARA_TYPEyNS_4lessIS1_EENS_9allocatorINS_4pairIKS1_yEEEEEE");
        am_gralloc_compose_slot_id = (am_gralloc_compose_slot_id_t)dlsym(gGrallocHandle, "_Z26am_gralloc_compose_slot_idj");
        if (am_gralloc_get_slot_id != NULL
            && am_gralloc_free_slot != NULL
            && am_gralloc_set_parameters != NULL
            && am_gralloc_compose_slot_id != NULL) {
            mGrallocVersion = 2;
        } else {
            dlclose(gGrallocHandle);
            gGrallocHandle = NULL;
        }
        CODEC2_LOG(CODEC2_LOG_DEBUG_LEVEL1, "[%s@%d] Gralloc version is:%d, grelloc handle[%p:%p:%p:%p:%p]",
                __func__, __LINE__, mGrallocVersion, gGrallocHandle, am_gralloc_get_slot_id,
                am_gralloc_free_slot, am_gralloc_set_parameters, am_gralloc_compose_slot_id);
    }


}

int32_t GrallocWraper::getSlotID() {
    LockWeakPtrWithReturnVal(comp, mComp, -1);
    if (mGrallocVersion < 2) {
        C2VdecGW_LOG(CODEC2_LOG_INFO, "Gralloc version:%d NOT supported this function", mGrallocVersion);
        return -1;
    }

    mSlotID = am_gralloc_get_slot_id();
    C2VdecGW_LOG(CODEC2_LOG_INFO, "Got SlotID:%d", mSlotID);
    return mSlotID;
}

void GrallocWraper::freeSlotID() {
    LockWeakPtrWithReturnVoid(comp, mComp);
    if (mSlotID < 0) {
        C2VdecGW_LOG(CODEC2_LOG_INFO, "slot id:%d NOT created", mSlotID);
        return;
    }

    C2VdecGW_LOG(CODEC2_LOG_INFO, "free slot:%d", mSlotID);
    am_gralloc_free_slot(mSlotID);
    mSlotID = -1;
}

void GrallocWraper::setParameters(C2VdecComponent::DeviceUtil* deviceUtil, const media::Size& size) {
    LockWeakPtrWithReturnVoid(comp, mComp);
    if (mSlotID < 0) {
        C2VdecGW_LOG(CODEC2_LOG_INFO, "slot id:%d NOT created", mSlotID);
        return;
    }

    am_gralloc_decode_para param;
    param[GRALLOC_DECODE_PARA_WIDTH] = static_cast<uint64_t>(size.width());
    param[GRALLOC_DECODE_PARA_HEIGHT] = static_cast<uint64_t>(size.height());
    param[GRALLOC_DECODE_PARA_WALIGN] = deviceUtil->getDecoderWidthAlign();
    if (param[GRALLOC_DECODE_PARA_WALIGN] == -1) {
        param[GRALLOC_DECODE_PARA_WALIGN] = DEFAULT_WIDTH_ALIGN_SIZE;
    }
    param[GRALLOC_DECODE_PARA_HALIGN] = DEFAULT_HEIGHT_ALIGN_SIZE;
    // surface texture or buffer mode
    // software decode p010 stream, we use PRIVATE_3 usage which mean height align is **1**
    if ((deviceUtil->mUseSurfaceTexture || deviceUtil->mNoSurface)
        && (deviceUtil->mIsYcbRP010Stream && !deviceUtil->mHwSupportP010)) {
        param[GRALLOC_DECODE_PARA_HALIGN] = 1;
    }

    C2VdecGW_LOG(CODEC2_LOG_INFO, "setParam WxH(%llu x %llu), Align(%llu : %llu) for slot id:%d",
            (unsigned long long)param[GRALLOC_DECODE_PARA_WIDTH], (unsigned long long)param[GRALLOC_DECODE_PARA_HEIGHT],
            (unsigned long long)param[GRALLOC_DECODE_PARA_WALIGN], (unsigned long long)param[GRALLOC_DECODE_PARA_HALIGN], mSlotID);
    am_gralloc_set_parameters(mSlotID, param);
}

uint64_t GrallocWraper::getUsageFromSlotId() {
    LockWeakPtrWithReturnVal(comp, mComp, 0);
    if (mSlotID < 0) {
        C2VdecGW_LOG(CODEC2_LOG_INFO, "slot id:%d NOT created", mSlotID);
        return 0;
    }

    C2VdecGW_LOG(CODEC2_LOG_DEBUG_LEVEL1, "get usage From slot id:%d", mSlotID);
    return am_gralloc_compose_slot_id(mSlotID);
}

}
