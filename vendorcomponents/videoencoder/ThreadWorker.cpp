/*
 **
 ** Copyright 2012 The Android Open Source Project
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

// #define LOG_NDEBUG 0
#define LOG_TAG "ThreadWorker"
#include <utils/Log.h>

#include <ThreadWorker.h>

namespace android {

ThreadWorker::ThreadWorker()
    : mExitRequested(false),
      mRunning(false),
      mThread(0) {
}

void ThreadWorker::requestExit() {
    AutoMutex l(mExitRequestLock);
    /// mRunning = false;
    mExitRequested = true;
}

bool ThreadWorker::exitRequested() {
    AutoMutex l(mExitRequestLock);
    return mExitRequested;
}

bool ThreadWorker::start(void *(*start_routine)(void *), void *arg) {
    AutoMutex l(mLock);

    {
        // Turns off exit request.
        if (mExitRequested && mRunning) {
            if (mThread != 0) {/*if still  on running, clear infos.*/
                ALOGE("pthread on exit wait clean\n");
                pthread_join(mThread, NULL);
                mThread = 0;
            }
            mRunning = false;
        }
        AutoMutex l(mExitRequestLock);
        mExitRequested = false;
    }

    if (mRunning) {
        ALOGE("Worker is already running ignore start.");
        return false;
    }

    int ret = pthread_create(&mThread, NULL, start_routine, arg);
    if (ret != 0) {
        ALOGE("pthread_create failed (err=%d)", ret);
        return false;
    }
    mRunning = true;
    return true;
}

bool ThreadWorker::stop() {
    AutoMutex l(mLock);
    if (mRunning) {
        requestExit();
        int ret = pthread_join(mThread, NULL);
        if (ret != 0) {
            ALOGW("pthread_join failed (err=%d)", ret);
        }
        mThread = 0;
        mRunning = false;
    }
    {
        // Turns off exit request.
        AutoMutex l(mExitRequestLock);
        mExitRequested = false;
    }
    return true;
}

bool ThreadWorker::isRunning() {
    AutoMutex l(mLock);
    return mRunning;
}

}  // namespace android
