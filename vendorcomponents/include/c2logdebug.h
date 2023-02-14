#ifndef _C2_LOG_DEBUG_H
#define _C2_LOG_DEBUG_H

#include <cutils/properties.h>
#include <stdint.h>

#include <utils/Log.h>
#include <cutils/trace.h>
#include <utils/Trace.h>
#include <inttypes.h>
#include <string.h>

static unsigned int gloglevel = 1;

#define CODEC2_LOG_ERR 0
#define CODEC2_LOG_INFO 1
#define CODEC2_LOG_TAG_BUFFER 2
#define CODEC2_LOG_DEBUG_LEVEL1 4
#define CODEC2_LOG_DEBUG_LEVEL2 8
#define CODEC2_LOG_TRACE 16

#define CODEC2_VENC_LOG_ERR    CODEC2_LOG_ERR
#define CODEC2_VENC_LOG_INFO   CODEC2_LOG_INFO
#define CODEC2_VENC_LOG_DEBUG   CODEC2_LOG_DEBUG_LEVEL1
#define CODEC2_VENC_LOG_TAG_BUFFER  CODEC2_LOG_TAG_BUFFER
#define CODEC2_VENC_LOG_TRACE  CODEC2_LOG_TRACE

#define CODEC2_LOG(level, f, s...) \
do { \
    if (level > CODEC2_LOG_ERR) { \
        if (level == CODEC2_LOG_INFO) \
            ALOGI(f, ##s); \
        else if (level == CODEC2_LOG_DEBUG_LEVEL1 || level == CODEC2_LOG_DEBUG_LEVEL2) \
            ALOGD(f, ##s); \
        else if (level == CODEC2_LOG_TAG_BUFFER) \
            ALOGD(f, ##s); \
    } else { \
        if (level == CODEC2_LOG_ERR) \
            ALOGE(f, ##s); \
    } \
} while(0)

#define CODEC2_ATRACE_INT32(tag, num) \
do { \
    if (gloglevel & CODEC2_LOG_TRACE) {\
        ATRACE_INT(tag, num); \
    } \
} while(0)

#define CODEC2_ATRACE_INT64(tag, num) \
do { \
    if (gloglevel & CODEC2_LOG_TRACE) {\
        ATRACE_INT64(tag, num); \
    } \
} while(0)

#define propGetInt(str,def) \
do { \
    char value[PROPERTY_VALUE_MAX] = {}; \
    if (property_get(str, value, NULL) > 0) { \
        *def = atoi(value); \
        ALOGI("%s set = %d\n", str, *def); \
    } else { \
        ALOGI("%s is not set used def = %d\n", str, *def); \
    } \
} while(0)

#endif
