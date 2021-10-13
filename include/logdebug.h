#ifndef _LOGDEBUG_H
#define _LOGDEBUG_H

#include <cutils/properties.h>
#include <stdint.h>

static unsigned int gloglevel = 1;

#define CODEC2_LOGDEBUG_PROPERTY "vendor.media.codec2.loglevels"
#define CODEC2_LOG_ERR 0
#define CODEC2_LOG_INFO 1
#define CODEC2_LOG_DEBUG_LEVEL1 2
#define CODEC2_LOG_DEBUG_LEVEL2 4
#define CODEC2_LOG_TRACE 16

#define CODEC2_LOG(level, f, s...) \
do { \
    if (level & gloglevel) { \
        if (level == CODEC2_LOG_INFO) \
            ALOGI(f, ##s); \
        else if (level == CODEC2_LOG_DEBUG_LEVEL1 || level == CODEC2_LOG_DEBUG_LEVEL2) \
            ALOGD(f, ##s); \
        } else { \
        if (level == CODEC2_LOG_ERR) \
            ALOGE(f, ##s); \
        } \
} while(0)

static void propGetInt(const char* str, unsigned int* def) {
    char value[PROPERTY_VALUE_MAX];
    if (property_get(str, value, NULL) > 0) {
        *def = atoi(value);
        ALOGI("%s set = %d\n", str, *def);
    } else {
        ALOGI("%s is not set used def = %d\n", str, *def);
    }
}

#endif
