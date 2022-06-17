#ifndef _C2VDA_TUNNEL_BUFFER_UTIL_H_
#define _C2VDA_TUNNEL_BUFFER_UTIL_H_

#include <C2Config.h>
#include <C2Enum.h>
#include <C2Param.h>
#include <C2ParamDef.h>

#include <VideoDecWraper.h>
#include <am_gralloc_ext.h>

namespace android {

class TunnelBufferUtil {
public:
    TunnelBufferUtil(std::shared_ptr<VideoDecWraper> wrap);
    virtual ~TunnelBufferUtil();

    c2_status_t fetchTunnelBuffer(
        uint32_t width, uint32_t height, uint32_t format,
        C2MemoryUsage usage,
        int* sharefd);

    c2_status_t freeTunnelBuffer(int fd);

private:
    std::shared_ptr<VideoDecWraper> mVideoDecWraper;

    enum {
        FULL_BUFFER_USAGE = 1,
        ONE_SIXTEENTH_BUFFER_USAGE = 2,
        QUARTER_BUFFER_USAGE = 3,
    };
};

}

#endif
