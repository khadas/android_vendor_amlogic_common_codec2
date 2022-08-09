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

#ifndef ANDROID_THREAD_WORKER_H_
#define ANDROID_THREAD_WORKER_H_

#include <utils/threads.h>

namespace android {

// TODO: move this somewhere common

/**
 * A general wrapper for running a worker thread.
 */
class ThreadWorker {
public:
    ThreadWorker();

    void requestExit();
    bool exitRequested();
    bool start(void *(*start_routine)(void *), void *arg);
    bool stop();
    bool isRunning();

private:
    bool mExitRequested;
    Mutex mExitRequestLock;  // protects mExitRequested
    Mutex mLock;             // protects mRunning and thread creation / deletion

    bool mRunning;
    // This is initialized with garbage, should always be used with mRunning
    pthread_t mThread;
};

}  // namespace android

#endif   // ANDROID_THREAD_WORKER_H_
