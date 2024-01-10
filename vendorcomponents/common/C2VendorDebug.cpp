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

#include <string.h>
#include <sstream>
#include <cstring>
#include <fcntl.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdlib.h>
#include <C2VendorDebug.h>
#include <C2VendorProperty.h>

#define LOG_NDEBUG 0
#define LOG_TAG "C2DBG"

namespace android {

static char* _trim(char* str) {
    int len = strlen(str);
    int i = len -1;
    while (i >= 0) {
        if (str[i] == ' ' || str[i] == '\t') {
            str[i] = 0;
        } else {
            break;
        }
        i--;
    }
    len = strlen(str);
    i = 0;
    while (i < len) {
        if (str[i] != ' ' && str[i] != '\t') {
            break;
        }
        i++;
    }
    return &str[i];
}

ResmanHandler::ResmanHandler():
        mResmanLibHandle(nullptr) {
    if (mResmanLibHandle == NULL) {
        mResmanLibHandle = dlopen("libmediahal_resman.so", RTLD_NOW);
        if (mResmanLibHandle) {
            resman_init = (fnResmanInit) dlsym(mResmanLibHandle, "resman_init");
            resman_close = (fnResmanClose) dlsym(mResmanLibHandle, "resman_close");
            resman_add_dumper = (fnResmanAddDumper) dlsym(mResmanLibHandle, "resman_add_handler_and_resreports");
            resman_add_debugger = (fnResmanAddDebugger) dlsym(mResmanLibHandle, "resman_add_debug_callback");

            if (!resman_init || !resman_close || !resman_add_dumper || !resman_add_debugger) {
                ALOGE("dlsym error:%s", dlerror());
                dlclose(mResmanLibHandle);
                mResmanLibHandle = NULL;
            }
        } else
            ALOGW("dlopen libmediahal_resman.so error:%s", dlerror());
    }
}

ResmanHandler::~ResmanHandler() {
    if (mResmanLibHandle)
        dlclose(mResmanLibHandle);
}

ANDROID_SINGLETON_STATIC_INSTANCE(C2DebugServer);

void onDump(void *instance) {
    C2DebugServer *server = (C2DebugServer*)instance;
    server->dump();
}

void onDebug(void *instance, const char *debug, int len) {
    C2DebugServer *server = (C2DebugServer*)instance;
    server->debug(debug, len);
}

C2DebugServer::C2DebugServer() {
    mResmanHandler = new ResmanHandler();
    if (mResmanHandler && mResmanHandler->isValid()) {
        mFd = mResmanHandler->resman_init("DEBUGGER", RESMAN_APP_DEBUG_SERVER);
        if (mResmanHandler->resman_add_dumper(mFd, onDump, onDump, (void *)this)) {
            ALOGE("Failed to resman_add_dumper");
        }
        if (mResmanHandler->resman_add_debugger(mFd, onDebug, (void *)this)) {
            ALOGE("Failed to resman_add_debugger");
        }
    }
}

C2DebugServer::~C2DebugServer() {
    if (isValid()) {
        ALOGD("%s fd = %d", __FUNCTION__, mFd);
        mResmanHandler->resman_close(mFd);
        mFd = -1;
        delete mResmanHandler;
        mResmanHandler = nullptr;
    }
}

void C2DebugServer::dump() {
    AutoMutex l(mLock);
    for (auto it = mClients.begin(); it != mClients.end(); it++) {
        auto module = it->first;
        auto clients = it->second;
        for (auto it2 = clients.begin(); it2 != clients.end(); it2++) {
            ALOGI("DUMP(%s) BEGIN", module.c_str());
            (*it2)->dump();
            ALOGI("DUMP(%s) END", module.c_str());
            ALOGI("\n--------------------------------\n");
        }
    }
}

void C2DebugServer::debug(const char *debug, int len) {
    char *cmd = strdup(debug);
    map<string, list<string>> commands;
    list<string> modules;
    commands.clear();
    modules.clear();
    char *kvtoken;
    kvtoken = strtok(cmd , ";");
    while (kvtoken) {
        if (strlen(kvtoken)) {
            string kv = string(kvtoken);
            kv.erase(0, kv.find_first_not_of(" \n\r\t"));
            kv.erase(kv.find_last_not_of(" \n\r\t")+1);
            int index = kv.find_first_of(":");
            if (index != string::npos && index > 0) {
                string m = kv.substr(0, index);
                m.erase(0, m.find_first_not_of(" \n\r\t"));
                m.erase(m.find_last_not_of(" \n\r\t")+1);
                if (!m.empty() && ++index < kv.length()) {
                    string c = kv.substr(index);
                    c.erase(0, c.find_first_not_of(" \n\r\t"));
                    c.erase(c.find_last_not_of(" \n\r\t")+1);
                    if (!m.empty() && !c.empty()) {
                        list<string> cs;
                        stringstream ss(c);
                        while (ss.good()) {
                            string s;
                            getline(ss, s, ',');
                            if (!s.empty()) {
                                s.erase(0, s.find_first_not_of(" \n\r\t"));
                                s.erase(s.find_last_not_of(" \n\r\t")+1);
                                if (!s.empty()) {
                                    cs.push_back(s);
                                }
                            }
                        }
                        if (!cs.empty()) {
                            if (commands.find(m) != commands.end()) {
                                commands[m].merge(cs);
                            } else {
                                commands[m] = cs;
                            }
                            modules.push_back(m);
                        }
                    }
                }
            }
        }
        kvtoken = strtok(NULL, ";");
    }
    free(cmd);
    AutoMutex l(mLock);
    for (auto it = modules.begin(); it != modules.end(); it++) {
        auto module = *it;
        auto cmds = commands[module];
        auto c = mClients.find(module);
        if (c != mClients.end()) {
            auto clients = c->second;
            for (auto it2 = clients.begin(); it2 != clients.end(); it2++) {
                ALOGI("DEBUG(%s) BEGIN", module.c_str());
                (*it2)->debug(cmds);
                ALOGI("DEBUG(%s) END", module.c_str());
            }
        }
    }
}

void C2DebugServer::addClient(const string &module, shared_ptr<IC2Debuggable> client) {
    AutoMutex l(mLock);
    ALOGD("adding client to %s", module.c_str());
    auto it = mClients.find(module);
    if (it == mClients.end()) {
        auto clients = list<shared_ptr<IC2Debuggable>>();
        clients.push_back(client);
        mClients[module] = clients;
    } else {
        auto clients = it->second;
        while (clients.size() >= kMaxClients) {
            clients.pop_front();
        }
        clients.push_back(client);
    }
}

ANDROID_SINGLETON_STATIC_INSTANCE(GlobalDebugger);

GlobalDebugger::GlobalDebugger() {
    mServer = &C2DebugServer::getInstance();
    mServer->addClient("codec2",  shared_ptr<IC2Debuggable>(this));
}

void GlobalDebugger::dump() {
#ifdef HAVE_VERSION_INFO
    static const char* c2Features = (char*)MEDIA_MODULE_FEATURES;
#endif

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
}

void GlobalDebugger::debug(list<string> cmds) {
    for (auto it = cmds.begin(); it != cmds.end(); it++) {
        string cmd = *it;
        if (strncmp(kCommandSetProp, cmd.c_str(), strlen(kCommandSetProp)) == 0) {
            string prop = cmd.substr(strlen(kCommandSetProp) + 1);
            prop.erase(0, prop.find_first_not_of(" \n\r\t"));
            prop.erase(prop.find_last_not_of(" \n\r\t")+1);
            size_t pos = prop.find(" ");
            if (pos != string::npos) {
                //trim the prop and value
                string value = prop.substr(pos + 1);
                prop = prop.substr(0, pos);
                value.erase(0, value.find_first_not_of(" \n\r\t"));
                value.erase(value.find_last_not_of(" \n\r\t")+1);
                ALOGD("set prop %s to %s", prop.c_str(), value.c_str());
                property_set(prop.c_str(), value.c_str());
            }
        } else {
            ALOGE("unknown debug command %s", cmd.c_str());
        }
    }
}

GlobalDebugger::~GlobalDebugger() {
}

ANDROID_SINGLETON_STATIC_INSTANCE(DefaultDebugger);

DefaultDebugger::DefaultDebugger() {
    mServer = &C2DebugServer::getInstance();
    mServer->addClient("default",  shared_ptr<IC2Debuggable>(this));
}

void DefaultDebugger::dump() {
}

void DefaultDebugger::debug(list<string> cmds) {
    for (auto it = cmds.begin(); it != cmds.end(); it++) {
        string cmd = *it;
        if (strncmp(kCommandLogLevel, cmd.c_str(), strlen(kCommandLogLevel)) == 0) {
            string logLevel = cmd.substr(strlen(kCommandLogLevel) + 1);
            logLevel.erase(0, logLevel.find_first_not_of(" \n\r\t"));
            logLevel.erase(logLevel.find_last_not_of(" \n\r\t")+1);
            ALOGD("set log level to %s", logLevel.c_str());
            property_set(CODEC2_LOGDEBUG_PROPERTY, logLevel.c_str());
        } else {
            ALOGE("unknown debug command %s", cmd.c_str());
        }
    }
}

DefaultDebugger::~DefaultDebugger() {
}

}
