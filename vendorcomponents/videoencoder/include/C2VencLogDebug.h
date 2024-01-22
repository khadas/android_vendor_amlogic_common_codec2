#ifndef _ENCODER_LOGDEBUG_H
#define _ENCODER_LOGDEBUG_H
#include <utils/Log.h>

static unsigned int gloglevel_encoder = 1;

#define CODEC2_VENC_LOG_ERR    CODEC2_LOG_ERR
#define CODEC2_VENC_LOG_INFO   CODEC2_LOG_INFO
#define CODEC2_VENC_LOG_DEBUG  CODEC2_LOG_DEBUG_LEVEL1
#define CODEC2_VENC_LOG_TAG_BUFFER  CODEC2_LOG_TAG_BUFFER
#define CODEC2_VENC_LOG_TRACE  CODEC2_LOG_TRACE

#define CODEC2_VENC_LOG(level, f, s...) \
do { \
        if (level & gloglevel_encoder) { \
            if (level == CODEC2_LOG_INFO) \
                ALOGI(f, ##s); \
            else if (level == CODEC2_LOG_DEBUG_LEVEL1 || level == CODEC2_LOG_DEBUG_LEVEL2) \
                ALOGD(f, ##s); \
            else if (level == CODEC2_LOG_TAG_BUFFER) \
                ALOGD(f, ##s); \
        } else { \
            if (level == CODEC2_LOG_ERR) \
            ALOGE(f, ##s); \
            }\
}while(0)

inline void compute_venc_loglevel() {
    gloglevel_encoder = (gloglevel_encoder <= android::gloglevel) ? android::gloglevel : gloglevel_encoder;
}
#endif

