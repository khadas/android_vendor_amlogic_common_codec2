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

#ifndef _C2VDEC_DEBUGUTIL_H_
#define _C2VDEC_DEBUGUTIL_H_

#include <stdint.h>
#include <inttypes.h>
#include <list>

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>
#include <utils/Log.h>
#include <utils/Singleton.h>
#include <utils/Timers.h>

#include <C2VdecComponent.h>

namespace android {

const int kSmoothnessFactor = 4;

static const int64_t kInvalidTimestamp = ((int64_t)-1);
static const int64_t MS = (1000LL);
static const int64_t Sec = (1000 * MS);
static const int64_t Minute = (60 * Sec);
static const int64_t Hour = (60 * Minute);
static const uint64_t kLogTimeout = 60*15;

#define GraphicBlockStateChange(comp, info, to) \
    do {\
        if (comp == NULL) { \
            CODEC2_LOG(CODEC2_LOG_ERR, "component is null, please check it."); \
            break; \
        }\
        if (info == NULL) { \
            CODEC2_LOG(CODEC2_LOG_ERR, "info is null, please check it."); \
            break; \
        }\
        bool stateChanged = false;\
        if (info->mState != to) {\
            stateChanged = true;\
            comp->mGraphicBlockStateCount[(int)info->mState] --;\
        }\
        if (stateChanged  && to < GraphicBlockInfo::State::GRAPHIC_BLOCK_OWNER_MAX) {\
            info->mState = to;\
            comp->mGraphicBlockStateCount[(int)info->mState] ++;\
        }\
    } while (0)

#define GraphicBlockStateInit(comp, info, state) \
    do {\
        if (comp == NULL) { \
            CODEC2_LOG(CODEC2_LOG_ERR, "component is null, please check it."); \
            break; \
        }\
        if (info == NULL) { \
            CODEC2_LOG(CODEC2_LOG_ERR, "info is null, please check it."); \
            break; \
        }\
        if (state < GraphicBlockInfo::State::GRAPHIC_BLOCK_OWNER_MAX) { \
            info->mState = state;\
            comp->mGraphicBlockStateCount[(int)info->mState] ++;\
        }\
    } while (0)

#define GraphicBlockStateReset(comp, info) \
    do {\
        if (comp == NULL) { \
            CODEC2_LOG(CODEC2_LOG_ERR, "component is null, please check it."); \
            break; \
        }\
        if (info == NULL) { \
            CODEC2_LOG(CODEC2_LOG_ERR, "info is null, please check it."); \
            break; \
        }\
        if (info->mState < GraphicBlockInfo::State::GRAPHIC_BLOCK_OWNER_MAX)\
            comp->mGraphicBlockStateCount[(int)info->mState] --;\
    } while (0)

#define GraphicBlockStateInc(comp, state) \
    do {\
        if (comp == NULL) { \
            CODEC2_LOG(CODEC2_LOG_ERR, "component is null, please check it."); \
            break; \
        }\
        if (state < GraphicBlockInfo::State::GRAPHIC_BLOCK_OWNER_MAX)\
            comp->mGraphicBlockStateCount[(int)state] ++;\
    } while (0)

#define GraphicBlockStateDec(comp, state) \
    do {\
        if (comp == NULL) { \
            CODEC2_LOG(CODEC2_LOG_ERR,"component is null, please check it."); \
            break; \
        } \
        if (state < GraphicBlockInfo::State::GRAPHIC_BLOCK_OWNER_MAX)\
            comp->mGraphicBlockStateCount[(int)state] --;\
    } while (0)

#define BufferStatus(comp, level, fmt, str...) \
    do {\
        if (comp == NULL) { \
            CODEC2_LOG(CODEC2_LOG_ERR, "component is null, please check it."); \
            break; \
        } \
        if (comp->isNonTunnelMode() && comp->mGraphicBlocks.empty() == false) \
            CODEC2_LOG(level, "[%d##%d]" fmt " {IN=%d/%d, OUT=%d/%zu[%s(%d) %s(%d) %s(%d)]}",\
                    comp->mSessionID,\
                    comp->mDecoderID, \
                    ##str, \
                    comp->mInputQueueNum,\
                    comp->mIntfImpl->mActualInputDelay->value + kSmoothnessFactor,\
                    comp->mGraphicBlockStateCount[(int)GraphicBlockInfo::State::OWNED_BY_CLIENT],\
                    comp->mGraphicBlocks.size(),\
                    comp->GraphicBlockState(GraphicBlockInfo::State::OWNED_BY_COMPONENT),\
                    comp->mGraphicBlockStateCount[(int)GraphicBlockInfo::State::OWNED_BY_COMPONENT],\
                    comp->GraphicBlockState(GraphicBlockInfo::State::OWNED_BY_ACCELERATOR),\
                    comp->mGraphicBlockStateCount[(int)GraphicBlockInfo::State::OWNED_BY_ACCELERATOR],\
                    comp->GraphicBlockState(GraphicBlockInfo::State::OWNED_BY_CLIENT),\
                    comp->mGraphicBlockStateCount[(int)GraphicBlockInfo::State::OWNED_BY_CLIENT]);\
        else if (comp->isTunnelMode() && comp->mGraphicBlocks.empty() == false)\
            CODEC2_LOG(level, "[%d##%d]" fmt " {IN=%d/%d, OUT=%d/%zu[%s(%d) %s(%d) %s(%d)]}",\
                    comp->mSessionID, \
                    comp->mDecoderID, \
                    ##str, \
                    comp->mInputQueueNum,\
                    comp->mIntfImpl->mActualInputDelay->value + kSmoothnessFactor,\
                    comp->mGraphicBlockStateCount[(int)GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER],\
                    comp->mGraphicBlocks.size(),\
                    comp->GraphicBlockState(GraphicBlockInfo::State::OWNED_BY_COMPONENT),\
                    comp->mGraphicBlockStateCount[(int)GraphicBlockInfo::State::OWNED_BY_COMPONENT],\
                    comp->GraphicBlockState(GraphicBlockInfo::State::OWNED_BY_ACCELERATOR),\
                    comp->mGraphicBlockStateCount[(int)GraphicBlockInfo::State::OWNED_BY_ACCELERATOR],\
                    comp->GraphicBlockState(GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER),\
                    comp->mGraphicBlockStateCount[(int)GraphicBlockInfo::State::OWNER_BY_TUNNELRENDER]);\
    } while (0)

enum RESMAN_APP {
    RESMAN_APP_SYSTEM_RESERVED = 100,
    RESMAN_APP_DIAGNOSTICS = 101,
};

typedef int (*DResman_init)(const char *appname, int type);
typedef int (*DResman_close)(int handle);
typedef int (*DResman_add_handler_and_resreports)(int fd, void (* handler)(void *), void (* resreport)(void *), void *opaque);

class ResmanHandler {
public:
    ResmanHandler();
    ~ResmanHandler();
    bool isValid() { return !!mResmanLibHandle; }
    DResman_init   Resman_init;
    DResman_close  Resman_close;
    DResman_add_handler_and_resreports Resman_add_handler_and_resreports;

private:
    void* mResmanLibHandle;
};

class AmlDiagnosticServer;
struct AmlDiagnosticStatsQty;

class C2VdecComponent::DebugUtil {
public:
    static constexpr char* kCreatedAt = (char*)   "Created At   :";
    static constexpr char* kStartedAt = (char*)   "Started At   :";
    static constexpr char* kStoppedAt = (char*)   "Stopped At   :";
    static constexpr char* kDestroyedAt = (char*) "Destroyed At :";
    static constexpr char* kInputStats  = (char*)  "Input Stats  :";
    static constexpr char* kOutputStats = (char*) "Output Stats :";

    DebugUtil();
    virtual ~DebugUtil();

    c2_status_t setComponent(std::shared_ptr<C2VdecComponent> sharecomp);
    void showGraphicBlockInfo();
    void startShowPipeLineBuffer();
    void showCurrentProcessFdInfo();

    void ctor();
    void start();
    void stop();
    void dtor();

    void emptyBuffer(void* buff, int64_t timestamp, uint32_t flags, uint32_t size);
    void emptyBufferDone(void* buff, void* buffer = nullptr, int64_t bitstreamId=-1);
    void fillBuffer(void* buff);
    void fillBufferDone(void* buff, uint32_t flags, uint32_t index,
                        int64_t timestamp=kInvalidTimestamp, int64_t bitstreamId=-1, int64_t pictureBufferId=-1);
    void dump();

private:
    std::weak_ptr<C2VdecComponent> mComp;
    std::weak_ptr<C2VdecComponent::IntfImpl> mIntfImpl;
    ::base::WeakPtrFactory<C2VdecComponent::DebugUtil> mWeakFactory;

    /* The singleton server */
    AmlDiagnosticServer* mServer;
    void resetStats();

    nsecs_t getNowUs();

    char mName[128];
    /* lifecycle stats */
    nsecs_t mCreatedAt;
    nsecs_t mStartedAt;
    nsecs_t mStoppedAt;
    nsecs_t mDestroyedAt;

    std::shared_ptr<AmlDiagnosticStatsQty> mInputQtyStats;
    std::shared_ptr<AmlDiagnosticStatsQty> mOutputQtyStats;
};

class AmlDiagnosticServer : public Singleton<AmlDiagnosticServer> {
public:
    static constexpr uint8_t kMaxClients = 1;
    AmlDiagnosticServer();
    ~AmlDiagnosticServer();
    void dump();
    void addClient(std::shared_ptr<C2VdecComponent::DebugUtil> client);
    bool isValid() { return mfd >= 0; }
private:
    int mfd;
    ResmanHandler *mResmanHandler;
    Mutex mLock;
    std::list<std::shared_ptr<C2VdecComponent::DebugUtil>> mClients;
};

struct AmlDiagnosticStatsQty {
    static constexpr nsecs_t kDistance1S    = 1000000;
    static constexpr nsecs_t kDistance500ms = 500000;
    static constexpr nsecs_t kDistance100ms = 100000;
    static constexpr nsecs_t kDistance50ms  = 50000;
    static constexpr nsecs_t kDistance40ms  = 40000;
    static constexpr nsecs_t kDistance30ms  = 30000;
    static constexpr nsecs_t kDistance20ms  = 20000;
    static constexpr nsecs_t kDistance10ms  = 10000;
    static constexpr nsecs_t kDistance5ms   = 5000;
    static constexpr nsecs_t kDistance1ms   = 1000;

    static constexpr char* kTotal = (char*) "Total Qty     :";
    static constexpr char* kAvgms = (char*) "Avg. Distance :";
    static constexpr char* k1S    = (char*) ">1sec      :";
    static constexpr char* k500ms = (char*) "500ms~1sec :";
    static constexpr char* k100ms = (char*) "100ms~500ms:";
    static constexpr char* k50ms  = (char*) "50ms~100ms :";
    static constexpr char* k40ms  = (char*) "40ms~50ms  :";
    static constexpr char* k30ms  = (char*) "30ms~40ms  :";
    static constexpr char* k20ms  = (char*) "20ms~30ms  :";
    static constexpr char* k10ms  = (char*) "10ms~20ms  :";
    static constexpr char* k5ms   = (char*) "5ms~10ms   :";
    static constexpr char* k1ms   = (char*) "1ms~5ms    :";
    static constexpr char* k0ms   = (char*) "<1ms       :";

    AmlDiagnosticStatsQty();
    virtual ~AmlDiagnosticStatsQty();

    void put(int64_t val);
    void resetStats();
    void dump();

private:
    uint64_t mQtyTotal;
    uint64_t mQty1S;
    uint64_t mQty500ms;
    uint64_t mQty100ms;
    uint64_t mQty50ms;
    uint64_t mQty40ms;
    uint64_t mQty30ms;
    uint64_t mQty20ms;
    uint64_t mQty10ms;
    uint64_t mQty5ms;
    uint64_t mQty1ms;
    uint64_t mQty0ms;

    int64_t mMinVal;
    int64_t mMaxVal;
    nsecs_t mFirstAt;
    nsecs_t mLastAt;
    nsecs_t mAvgDistanceMs;

    nsecs_t getNowUs();
};

}
#endif
