/*
 * Copyright 2018 The Android Open Source Project
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
#define LOG_TAG "android.hardware.media.c2@1.1-service"

#include <C2VDASupport.h>

#include <android-base/logging.h>
#include <binder/ProcessState.h>
#include <codec2/hidl/1.1/ComponentStore.h>
#include <hidl/HidlTransportSupport.h>
#include <minijail.h>

#include <util/C2InterfaceHelper.h>
#include <C2Component.h>
#include <C2Config.h>

// This is the absolute on-device path of the prebuild_etc module
// "android.hardware.media.c2@1.1-default-seccomp_policy" in Android.bp.
static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/android.hardware.amlogic.media.c2@1.1-seccomp-policy";


// Additional seccomp permissions can be added in this file.
// This file does not exist by default.
static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/android.hardware.amlogic.media.c2@1.1-extended-seccomp-policy";

int main(int /* argc */, char** /* argv */) {
    using namespace ::android;
    LOG(DEBUG) << "android.hardware.media.c2@1.1-service starting...";

    // Set up minijail to limit system calls.
    signal(SIGPIPE, SIG_IGN);
    SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);

    // Enable vndbinder to allow vendor-to-vendor binder calls.
    ProcessState::initWithDriver("/dev/vndbinder");
    ProcessState::self()->startThreadPool();
    // Extra threads may be needed to handle a stacked IPC sequence that
    // contains alternating binder and hwbinder calls. (See b/35283480.)
    hardware::configureRpcThreadpool(8, true /* callerWillJoin */);

    // Create IComponentStore service.
    {
        using namespace ::android::hardware::media::c2::V1_1;
        android::sp<IComponentStore> store;

        LOG(DEBUG) << "Instantiating Codec2's VDA IComponentStore service...";
        store = new utils::ComponentStore(android::GetCodec2VDAComponentStore());

        if (store == nullptr) {
           LOG(ERROR) << "Cannot create Codec2's VDA IComponentStore service.";
        } else {
            constexpr char const* serviceName = "default";
            if (store->registerAsService(serviceName) != android::OK) {
                LOG(ERROR) << "Cannot register Codec2's IComponentStore service."
                            " with instance name << \""
                            << serviceName << "\".";
            } else {
                LOG(ERROR) << "Codec2's IComponentStore default service created.";
            }
        }
    }

    android::hardware::joinRpcThreadpool();
    return 0;
}
