/*
 * Copyright 2018, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VdaBlockPool"

#include <errno.h>

#include <mutex>

#include <utils/Log.h>

#include <C2AllocatorGralloc.h>
#include <C2BlockInternal.h>
#include <C2PlatformSupport.h>
#include <utils/CallStack.h>

#include <hidl/HidlSupport.h>
#include <ui/BufferQueueDefs.h>
#include <ui/GraphicBuffer.h>
#include <ui/Fence.h>

#include <types.h>
#include <utils/CallStack.h>
#include <C2VDASupport.h>
#include <hardware/gralloc1.h>
#include "C2VdaBqBlockPool.h"
#include "C2VdaPooledBlockPool.h"

using namespace android;

static C2BlockPool* sPool = NULL;
static C2AllocatorGralloc* sAllocator = NULL;

extern "C" C2BlockPool* CreateBlockPool(::C2Allocator::id_t allocatorId,
    ::C2BlockPool::local_id_t blockPoolId) {
    ALOGI("CreateBlockPool allocatorId %d, blockPoolId %llu, sPool %p\n", allocatorId, blockPoolId, sPool);
    switch (allocatorId) {
        case android::C2VDAAllocatorStore::V4L2_BUFFERQUEUE: {
            ALOGI("new C2VdaBqBlockPool\n");
            std::shared_ptr<C2AllocatorGralloc> alloc =
                std::make_shared<C2AllocatorGralloc>(android::C2PlatformAllocatorStore::BUFFERQUEUE);
            sPool = new C2VdaBqBlockPool(std::move(alloc), blockPoolId);
            break;
        }
        case android::C2VDAAllocatorStore::V4L2_BUFFERPOOL:{
            ALOGI("new C2VdaPooledBlockPool\n");
            std::shared_ptr<C2AllocatorGralloc> alloc =
                std::make_shared<C2AllocatorGralloc>(android::C2PlatformAllocatorStore::GRALLOC);
            sPool = new C2VdaPooledBlockPool(std::move(alloc), blockPoolId);
            break;
        }
    }

    return sPool;
}

extern "C" C2Allocator* CreateAllocator(::C2Allocator::id_t id, ::c2_status_t* res) {
    (void)(id);
    ALOGI("CreateAllocator id %d\n", id);
    sAllocator = new C2AllocatorGralloc(id);
    *res = C2_OK;
    return sAllocator;
}

