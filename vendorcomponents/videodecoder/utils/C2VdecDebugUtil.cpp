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
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
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

#define DUMP_PROCESS_FDINFO_ENABLE (0)

namespace android {
#define RETURN_ON_UNINITIALIZED_OR_ERROR()                                 \
    do {                                                                   \
        if (comp->mHasError \
            || comp->mComponentState == ComponentState::UNINITIALIZED \
            || comp->mComponentState == ComponentState::DESTROYING \
            || comp->mComponentState == ComponentState::DESTROYED) \
            return;                                                        \
    } while (0)

#define C2VdecDU_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, comp->mCurInstanceID, C2VdecComponent::mInstanceNum, ##str)

#define LockWeakPtrWithReturnVal(name, weak, retval) \
    auto name = weak.lock(); \
    if (name == nullptr) { \
        C2VdecDU_LOG(CODEC2_LOG_ERR, "[%s:%d] null ptr, please check", __func__, __LINE__); \
        return retval;\
    }

#define LockWeakPtrWithReturnVoid(name, weak) \
    auto name = weak.lock(); \
    if (name == nullptr) { \
        C2VdecDU_LOG(CODEC2_LOG_ERR, "[%s:%d] null ptr, please check", __func__, __LINE__); \
        return;\
    }

#define LockWeakPtrWithoutReturn(name, weak) \
    auto name = weak.lock(); \
    if (name == nullptr) { \
        CODEC2_LOG(CODEC2_LOG_ERR, "[%s:%d] null ptr, please check", __func__, __LINE__); \
    }


C2VdecComponent::DebugUtil::DebugUtil() {
    propGetInt(CODEC2_VDEC_LOGDEBUG_PROPERTY, &gloglevel);
    CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
}

C2VdecComponent::DebugUtil::~DebugUtil() {
    CODEC2_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);
}

c2_status_t C2VdecComponent::DebugUtil::setComponent(std::shared_ptr<C2VdecComponent> sharedcomp) {
    mComp = sharedcomp;
    LockWeakPtrWithReturnVal(comp, mComp, C2_BAD_VALUE);
    mIntfImpl = comp->GetIntfImpl();
    C2VdecDU_LOG(CODEC2_LOG_INFO, "[%s:%d]", __func__, __LINE__);

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
    RETURN_ON_UNINITIALIZED_OR_ERROR();

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
        ::base::Bind(&C2VdecComponent::DebugUtil::startShowPipeLineBuffer, ::base::Unretained(this)),
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

}
