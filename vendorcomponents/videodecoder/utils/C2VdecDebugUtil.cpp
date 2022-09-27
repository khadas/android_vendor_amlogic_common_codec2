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

#include <c2logdebug.h>
#include <C2VdecDebugUtil.h>
#include <C2VdecInterfaceImpl.h>

namespace android {
#define RETURN_ON_UNINITIALIZED_OR_ERROR()                                 \
    do {                                                                   \
        if (mComp->mHasError \
            || mComp->mComponentState == ComponentState::UNINITIALIZED \
            || mComp->mComponentState == ComponentState::DESTROYING \
            || mComp->mComponentState == ComponentState::DESTROYED) \
            return;                                                        \
    } while (0)

#define C2VdecDU_LOG(level, fmt, str...) CODEC2_LOG(level, "[%d##%d]"#fmt, C2VdecComponent::mInstanceID, mComp->mCurInstanceID, ##str)

C2VdecComponent::DebugUtil::DebugUtil(C2VdecComponent* comp):
    mComp(comp) {
    DCHECK(mComp != NULL);
    mTaskRunner = mComp->GetTaskRunner();
    DCHECK(mTaskRunner != NULL);
    mIntfImpl = mComp->GetIntfImpl();
    DCHECK(mIntfImpl != NULL);

    propGetInt(CODEC2_LOGDEBUG_PROPERTY, &gloglevel);
}

C2VdecComponent::DebugUtil::~DebugUtil() {

}

void C2VdecComponent::DebugUtil::showGraphicBlockInfo() {
    for (auto & blockItem : mComp->mGraphicBlocks) {
        C2VdecDU_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] PoolId(%d) BlockId(%d) Fd(%d) State(%s) Blind(%d) FdHaveSet(%d) UseCount(%ld)",
             __func__, blockItem.mPoolId, blockItem.mBlockId,
            blockItem.mFd, mComp->GraphicBlockState(blockItem.mState),
            blockItem.mBind, blockItem.mFdHaveSet, blockItem.mGraphicBlock.use_count());
    }
}

void C2VdecComponent::DebugUtil::startShowPipeLineBuffer() {
    RETURN_ON_UNINITIALIZED_OR_ERROR();
    BufferStatus(mComp, CODEC2_LOG_INFO, "in/out status {INS/OUTS=%" PRId64 "(%d)/%" PRId64 "(%zu)}, pipeline status",
            mComp->mInputWorkCount, mComp->mInputCSDWorkCount,
            mComp->mOutputWorkCount, mComp->mPendingBuffersToWork.size());
#if 0
    BufferStatus(mComp, CODEC2_LOG_INFO, "pipeline status");
    C2Vdec_LOG(CODEC2_LOG_INFO, "in/out status {INS/OUTS=%" PRId64"(%d)/%" PRId64"(%zu)}",
            mComp->mInputWorkCount, mComp->mInputCSDWorkCount,
            mComp->mOutputWorkCount, mComp->mPendingBuffersToWork.size());
#endif
    mTaskRunner->PostDelayedTask(FROM_HERE,
        ::base::Bind(&C2VdecComponent::DebugUtil::startShowPipeLineBuffer, ::base::Unretained(this)),
        base::TimeDelta::FromMilliseconds(5000));
}

void C2VdecComponent::DebugUtil::showCurrentProcessFdInfo() {
    int iPid = (int)getpid();
    std::string path;
    struct dirent *dir_info;
    path.append("/proc/" + std::to_string(iPid) + "/fdinfo/");
    DIR *dir= opendir(path.c_str());
    dir_info = readdir(dir);
    while (dir_info != NULL) {
        if (strcmp(dir_info->d_name, ".") != 0 && strcmp(dir_info->d_name, "..") != 0) {
            std::string p = path;
            p.append(dir_info->d_name);

            std::ifstream  srcFile(p.c_str(), std::ios::in | std::ios::binary);
            std::string data;
            if (!srcFile.fail()) {
                std::string tmp;
                while (srcFile.peek() != EOF) {
                    srcFile >> tmp;
                    data.append(" " + tmp);
                    tmp.clear();
                }
            }
            srcFile.close();
            C2VdecDU_LOG(CODEC2_LOG_DEBUG_LEVEL2, "[%s] info: fd(%s) %s", __func__ ,dir_info->d_name, data.c_str());
        }
        dir_info = readdir(dir);
    }
}

}
