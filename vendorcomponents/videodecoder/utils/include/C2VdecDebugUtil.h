/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
#ifndef _C2VDEC_DEBUGUTIL_H_
#define _C2VDEC_DEBUGUTIL_H_

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>
#include <SimpleC2Interface.h>
#include <util/C2InterfaceHelper.h>
#include <C2VdecComponent.h>

namespace android {

const int kSmoothnessFactor = 4;

#define GraphicBlockStateChange(comp, info, to) \
    do {\
        bool stateChanged = false;\
        if (info->mState != to) {\
            stateChanged = true;\
            comp->mGraphicBlockStateCount[(int)info->mState] --;\
        }\
        if (stateChanged) {\
            info->mState = to;\
            comp->mGraphicBlockStateCount[(int)info->mState] ++;\
        }\
    } while (0)

#define GraphicBlockStateInit(comp, info, state) \
    do {\
        info->mState = state;\
        comp->mGraphicBlockStateCount[(int)info->mState] ++;\
    } while (0)

#define GraphicBlockStateReset(comp, info) \
    do {\
        comp->mGraphicBlockStateCount[(int)info->mState] --;\
    } while (0)

#define GraphicBlockStateInc(comp, state) \
    do {\
        comp->mGraphicBlockStateCount[(int)state] ++;\
    } while (0)

#define GraphicBlockStateDec(comp, state) \
    do {\
        comp->mGraphicBlockStateCount[(int)state] --;\
    } while (0)

#define BufferStatus(comp, level, fmt, str...) \
    do {\
        if (comp->isNonTunnelMode()) \
            CODEC2_LOG(level, fmt" {IN=%d/%d, OUT=%d/%d[%s(%d) %s(%d) %s(%d)]}",\
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
        else if (comp->isTunnelMode())\
            CODEC2_LOG(level, fmt" {IN=%d/%d, OUT=%d/%d[%s(%d) %s(%d) %s(%d)]}",\
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


class C2VdecComponent::DebugUtil {
public:
    DebugUtil(C2VdecComponent* comp);
    virtual ~DebugUtil();

    void showGraphicBlockInfo();
    void startShowPipeLineBuffer();
    void showCurrentProcessFdInfo();

private:
    C2VdecComponent* mComp;
    C2VdecComponent::IntfImpl* mIntfImpl;
    scoped_refptr<::base::SingleThreadTaskRunner> mTaskRunner;
};
}
#endif
