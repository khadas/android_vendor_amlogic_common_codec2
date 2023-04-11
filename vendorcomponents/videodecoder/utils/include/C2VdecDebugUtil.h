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
                    comp->mCurInstanceID, \
                    mInstanceNum, \
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
                    comp->mCurInstanceID, \
                    mInstanceNum, \
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
    DebugUtil();
    virtual ~DebugUtil();

    c2_status_t setComponent(std::shared_ptr<C2VdecComponent> sharecomp);
    void showGraphicBlockInfo();
    void startShowPipeLineBuffer();
    void showCurrentProcessFdInfo();

private:
    std::weak_ptr<C2VdecComponent> mComp;
    std::weak_ptr<C2VdecComponent::IntfImpl> mIntfImpl;
};
}
#endif
