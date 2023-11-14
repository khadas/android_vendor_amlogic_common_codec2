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
#define LOG_TAG "DebugUtil"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <dlfcn.h>
#include <dirent.h>
#include <fstream>
#include <inttypes.h>
#include <string.h>
#include <algorithm>
#include <utils/Log.h>
#include <cutils/properties.h>
#include <base/bind.h>
#include <base/bind_helpers.h>

#include <C2VendorProperty.h>
#include <c2logdebug.h>
#include <C2VdecDebugUtil.h>
#include <C2VdecInterfaceImpl.h>

#include "base/memory/weak_ptr.h"

#define DUMP_PROCESS_FDINFO_ENABLE (0)
#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

using namespace std;
using namespace android;

namespace android {

#define C2VdecDU_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, comp->mSessionID, comp->mDecoderID, ##str)

C2VdecComponent::DebugUtil::DebugUtil():mWeakFactory(this) {
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
}

C2VdecComponent::DebugUtil::~DebugUtil() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
}

c2_status_t C2VdecComponent::DebugUtil::setComponent(std::shared_ptr<C2VdecComponent> sharedcomp) {
    mComp = sharedcomp;
    mIntfImpl = sharedcomp->GetIntfImpl();
    CODEC2_LOG(CODEC2_LOG_INFO, "[%d##%d][%s:%d]", sharedcomp->mSessionID, sharedcomp->mDecoderID, __func__, __LINE__);

    mServer = &AmlDiagnosticServer::getInstance();
    sprintf(mName, "%s", sharedcomp->mName.c_str());
    mCreatedAt = kInvalidTimestamp;
    mStartedAt = kInvalidTimestamp;
    mStoppedAt = kInvalidTimestamp;
    mDestroyedAt = kInvalidTimestamp;
    mInputQtyStats = make_shared<AmlDiagnosticStatsQty>();
    mOutputQtyStats = make_shared<AmlDiagnosticStatsQty>();
    resetStats();
    ctor();
    mServer->addClient(sharedcomp->mDebugUtil);
    return C2_OK;
}

void C2VdecComponent::DebugUtil::showGraphicBlockInfo() {
    LockWeakPtrWithReturnVoid(comp, mComp);
    for (auto & blockItem : comp->mGraphicBlocks) {
        C2VdecDU_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] PoolId(%d) BlockId(%d) Fd(%d) State(%s) Blind(%d) FdHaveSet(%d) UseCount(%ld)",
             __func__, blockItem.mPoolId, blockItem.mBlockId,
            blockItem.mFd, comp->GraphicBlockState(blockItem.mState),
            blockItem.mBind, blockItem.mFdHaveSet, blockItem.mGraphicBlock.use_count());
    }
}

void C2VdecComponent::DebugUtil::startShowPipeLineBuffer() {
    LockWeakPtrWithReturnVoid(comp, mComp);
    scoped_refptr<::base::SingleThreadTaskRunner> taskRunner = comp->GetTaskRunner();
    if (taskRunner == nullptr) {
        return;
    }

    BufferStatus(comp, CODEC2_LOG_INFO, "in/out status {INS/OUTS=%" PRId64 "(%d)/%" PRId64 "(%zu)}, pipeline status",
        comp->mInputWorkCount, comp->mInputCSDWorkCount,
        comp->mOutputWorkCount, comp->mPendingBuffersToWork.size());
#if 0
    BufferStatus(comp, CODEC2_LOG_INFO, "pipeline status");
    C2Vdec_LOG(CODEC2_LOG_INFO, "in/out status {INS/OUTS=%" PRId64"(%d)/%" PRId64"(%zu)}",
            comp->mInputWorkCount, comp->mInputCSDWorkCount,
            comp->mOutputWorkCount, comp->mPendingBuffersToWork.size());
#endif
    taskRunner->PostDelayedTask(FROM_HERE,
        ::base::Bind(&C2VdecComponent::DebugUtil::startShowPipeLineBuffer, mWeakFactory.GetWeakPtr()),
        ::base::TimeDelta::FromMilliseconds(5000));
}

void C2VdecComponent::DebugUtil::showCurrentProcessFdInfo() {
#if DUMP_PROCESS_FDINFO_ENABLE
    LockWeakPtrWithReturnVoid(comp, mComp);
    int iPid = (int)getpid();
    std::string path;
    path.append("/proc/" + std::to_string(iPid) + "/fdinfo/");
    DIR *dir= opendir(path.c_str());
    if (dir != NULL) {
        struct dirent * dir_info = readdir(dir);
        while (dir_info != NULL) {
            if (strcmp(dir_info->d_name, ".") != 0 && strcmp(dir_info->d_name, "..") != 0) {
                std::string dirPath;
                dirPath.append(path.append(dir_info->d_name).c_str());
                std::ifstream srcFile(dirPath.c_str(), std::ios::in);
                if (!srcFile.is_open()) {
                    C2VdecDU_LOG(CODEC2_LOG_ERR, "[%s] open file failed.", __func__);
                    closedir(dir);
                    return;
                }

                std::string fdInfo;
                if (!srcFile.fail()) {
                    while (srcFile.peek() != EOF) {
                        char temp[32] = {0};
                        srcFile.getline(temp, 32);
                        fdInfo.append(temp);
                    }
                }

                srcFile.close();
                C2VdecDU_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] info: fd(%s) %s", __func__ ,dir_info->d_name, fdInfo.c_str());
            }
            dir_info = readdir(dir);
        }
    }
    closedir(dir);
#endif
}

nsecs_t C2VdecComponent::DebugUtil::getNowUs() {
    return (systemTime(SYSTEM_TIME_MONOTONIC) / 1000LL);
}

void C2VdecComponent::DebugUtil::resetStats() {
    mInputQtyStats->resetStats();
    mOutputQtyStats->resetStats();
}

void C2VdecComponent::DebugUtil::dump() {
    nsecs_t now = getNowUs();
    ALOGI("%s\n"
          "%s %" PRIi64 "(secs ago)\n"
          "%s %" PRIi64 "(secs ago)\n"
          "%s %" PRIi64 "(secs ago)\n"
          "%s %" PRIi64 "(secs ago)\n",
          mName,
          kCreatedAt, (mCreatedAt != kInvalidTimestamp ? ((now - mCreatedAt)/Sec) : kInvalidTimestamp),
          kStartedAt, (mStartedAt != kInvalidTimestamp ? ((now - mStartedAt)/Sec) : kInvalidTimestamp),
          kStoppedAt, (mStoppedAt != kInvalidTimestamp ? ((now - mStoppedAt)/Sec) : kInvalidTimestamp),
          kDestroyedAt, (mDestroyedAt != kInvalidTimestamp ? ((now - mDestroyedAt)/Sec) : kInvalidTimestamp));
    ALOGI("%s\n", kInputStats);
    mInputQtyStats->dump();
    ALOGI("%s\n", kOutputStats);
    mOutputQtyStats->dump();
}

void C2VdecComponent::DebugUtil::ctor() {
    mCreatedAt = getNowUs();
}

void C2VdecComponent::DebugUtil::start() {
    mStartedAt = getNowUs();
}

void C2VdecComponent::DebugUtil::stop() {
    mStoppedAt = getNowUs();
}

void C2VdecComponent::DebugUtil::dtor() {
    mDestroyedAt = getNowUs();
}

void C2VdecComponent::DebugUtil::emptyBuffer(void* buffHdr, int64_t timestamp, uint32_t flags, uint32_t size) {
    UNUSED(buffHdr);
    UNUSED(timestamp);
    UNUSED(flags);
    UNUSED(size);
    mInputQtyStats->put(timestamp);
}

void C2VdecComponent::DebugUtil::emptyBufferDone(void* buffHdr, void* buffer, int64_t bitstreamId) {
    UNUSED(buffHdr);
    UNUSED(buffer);
    UNUSED(bitstreamId);
    //TODO: doing nothing now
}

void C2VdecComponent::DebugUtil::fillBuffer(void* buffHdr) {
    UNUSED(buffHdr);
    //TODO: doing nothing now
}

void C2VdecComponent::DebugUtil::fillBufferDone(void *buffHdr, uint32_t flags, uint32_t index, int64_t timestamp,
                                         int64_t bitstreamId, int64_t pictureBufferId) {
    UNUSED(buffHdr);
    UNUSED(flags);
    UNUSED(index);
    UNUSED(timestamp);
    UNUSED(bitstreamId);
    UNUSED(pictureBufferId);
    if (pictureBufferId != -1 && bitstreamId != -1) {
        mOutputQtyStats->put(timestamp);
    }
}

ResmanHandler::ResmanHandler():
        mResmanLibHandle(nullptr) {
    if (mResmanLibHandle == NULL) {
        mResmanLibHandle = dlopen("libmediahal_resman.so", RTLD_NOW);
        if (mResmanLibHandle) {
            Resman_init = (DResman_init) dlsym(mResmanLibHandle, "resman_init");
            Resman_close = (DResman_close) dlsym(mResmanLibHandle, "resman_close");
            Resman_add_handler_and_resreports = (DResman_add_handler_and_resreports) dlsym(mResmanLibHandle, "resman_add_handler_and_resreports");

            if (!Resman_init || !Resman_close || !Resman_add_handler_and_resreports) {
                ALOGE("dlsym error:%s", dlerror());
                dlclose(mResmanLibHandle);
                mResmanLibHandle = NULL;
            }
        } else
            ALOGW("dlopen libmediahal_resman.so error:%s", dlerror());
    }
}

ResmanHandler::~ResmanHandler()
{
    if (mResmanLibHandle)
        dlclose(mResmanLibHandle);
}

ANDROID_SINGLETON_STATIC_INSTANCE(AmlDiagnosticServer);

void onDumpboardCallback(void *instance) {
    AmlDiagnosticServer *dumpboard = (AmlDiagnosticServer*)instance;
    dumpboard->dump();
}

AmlDiagnosticServer::AmlDiagnosticServer() {
    mResmanHandler = new ResmanHandler();
    if (mResmanHandler && mResmanHandler->isValid()) {
        mfd = mResmanHandler->Resman_init("DumpState", RESMAN_APP_DIAGNOSTICS);
        mResmanHandler->Resman_add_handler_and_resreports(mfd, onDumpboardCallback, onDumpboardCallback, (void *)this);
    }
}

AmlDiagnosticServer::~AmlDiagnosticServer() {
    if (isValid()) {
        ALOGD("%s fd = %d", __FUNCTION__, mfd);
        mResmanHandler->Resman_close(mfd);
        mfd = -1;
        delete mResmanHandler;
        mResmanHandler = nullptr;
    }
}

void AmlDiagnosticServer::dump() {
#ifdef HAVE_VERSION_INFO
    static const char* c2Features = (char*)MEDIA_MODULE_FEATURES;
#endif
    ALOGI("DUMPING DIAGNOSTICS DATABASE START");
#ifdef HAVE_VERSION_INFO
    ALOGI("\n--------------------------------\n"
        "ARCH = %s\n"
        "Version:%s\n"
        "%s\n"
        "%s\n"
        "Change-Id:%s\n"
        "CommitID:%s\n"
        "--------------------------------\n",
#if defined(__aarch64__)
        "arm64",
#else
        "arm",
#endif
        VERSION,
        GIT_COMMITMSG,
        GIT_PD,
        GIT_CHANGEID,
        GIT_COMMITID);
ALOGI("%s", c2Features);
#endif
    {
        AutoMutex l(mLock);
        for (auto client: mClients) {
            client->dump();
            ALOGI("\n--------------------------------\n");
        }
    }
    ALOGI("DUMPING DIAGNOSTICS DATABASE END");
}

void AmlDiagnosticServer::addClient(std::shared_ptr<C2VdecComponent::DebugUtil> client) {
    AutoMutex l(mLock);
    while (mClients.size() >= kMaxClients) {
        mClients.pop_front();
    }
    mClients.push_back(client);
}

AmlDiagnosticStatsQty::AmlDiagnosticStatsQty() {
    resetStats();
}

AmlDiagnosticStatsQty::~AmlDiagnosticStatsQty() {
}

void AmlDiagnosticStatsQty::put(int64_t val) {
    if (val < mMinVal) {
        mMinVal = val;
    }
    if (val > mMaxVal) {
        mMaxVal = val;
    }

    mQtyTotal++;
    nsecs_t now = getNowUs();
    if (mLastAt != kInvalidTimestamp) {
        nsecs_t distance = now - mLastAt;
        if (distance > kDistance1S) {
            mQty1S++;
        } else if (distance >  kDistance500ms)  {
            mQty500ms++;
        } else if (distance > kDistance100ms) {
            mQty100ms++;
        } else if (distance > kDistance50ms) {
            mQty50ms++;
        } else if (distance > kDistance40ms) {
            mQty40ms++;
        } else if (distance > kDistance30ms) {
            mQty30ms++;
        } else if (distance > kDistance20ms) {
            mQty20ms++;
        } else if (distance > kDistance10ms) {
            mQty10ms++;
        } else if (distance > kDistance5ms) {
            mQty5ms++;
        } else if (distance > kDistance1ms) {
            mQty1ms++;
        } else {
            mQty0ms++;
        }
        mAvgDistanceMs = (now - mFirstAt)/(mQtyTotal - 1)/MS;
    } else {
        mFirstAt = now;
    }
    mLastAt = now;
}

nsecs_t AmlDiagnosticStatsQty::getNowUs() {
    return (systemTime(SYSTEM_TIME_MONOTONIC) / 1000LL);
}

void AmlDiagnosticStatsQty::resetStats() {
    mMinVal = numeric_limits<int64_t>::max();
    mMaxVal = numeric_limits<int64_t>::min();
    mFirstAt = kInvalidTimestamp;
    mLastAt = kInvalidTimestamp;
    mAvgDistanceMs = kInvalidTimestamp;
    mQtyTotal = 0;
    mQty1S = 0;
    mQty500ms = 0;
    mQty100ms = 0;
    mQty50ms = 0;
    mQty40ms = 0;
    mQty30ms = 0;
    mQty20ms = 0;
    mQty10ms = 0;
    mQty5ms = 0;
    mQty1ms = 0;
    mQty0ms = 0;
}

void AmlDiagnosticStatsQty::dump() {
    ALOGI("  %s%" PRIu64 "\n"
          "  %s%" PRIi64 "ms\n"
          "    %s %" PRIu64 "\n"
          "    %s %" PRIu64 "\n"
          "    %s %" PRIu64 "\n"
          "    %s %" PRIu64 "\n"
          "    %s %" PRIu64 "\n"
          "    %s %" PRIu64 "\n"
          "    %s %" PRIu64 "\n"
          "    %s %" PRIu64 "\n"
          "    %s %" PRIu64 "\n"
          "    %s %" PRIu64 "\n"
          "    %s %" PRIu64 "\n",
          kTotal, mQtyTotal,
          kAvgms, mAvgDistanceMs,
          k1S, mQty1S,
          k500ms, mQty500ms,
          k100ms, mQty100ms,
          k50ms, mQty50ms,
          k40ms, mQty40ms,
          k30ms, mQty30ms,
          k20ms, mQty20ms,
          k10ms, mQty10ms,
          k5ms, mQty5ms,
          k1ms, mQty1ms,
          k0ms, mQty0ms);
}

}
