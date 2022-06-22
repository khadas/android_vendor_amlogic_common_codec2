#define LOG_NDEBUG 0

#define LOG_TAG "TunnelBufferUtil"

//#include <logdebug.h>
#include <C2VDATunnelBufferUtil.h>

namespace android {

#define OUTPUT_BUFS_ALIGN_SIZE (64)
#define OUT_BUFFER_ALIGIN(x) (((x) + OUTPUT_BUFS_ALIGN_SIZE - 1) & (~(OUTPUT_BUFS_ALIGN_SIZE - 1)))

TunnelBufferUtil::TunnelBufferUtil(std::shared_ptr<VideoDecWraper> wrap):
    mVideoDecWraper(wrap) {
    DCHECK(mVideoDecWraper != NULL);
    //propGetInt(CODEC2_LOGDEBUG_PROPERTY, &gloglevel);
    //CODEC2_LOG(CODEC2_LOG_INFO, "%s:%d", __func__, __LINE__);
}

TunnelBufferUtil::~TunnelBufferUtil() {
    mVideoDecWraper.reset();
    mVideoDecWraper = NULL;
}

c2_status_t TunnelBufferUtil::fetchTunnelBuffer(
    uint32_t width, uint32_t height, uint32_t format,
    C2MemoryUsage usage,
    int* sharefd) {
    (void)format;
    DCHECK(mVideoDecWraper != NULL);
    int halUsage = 0;
    C2AndroidMemoryUsage androidUsage = usage;
    uint64_t grallocUsage = androidUsage.asGrallocUsage();

    bool secure = ((grallocUsage & C2MemoryUsage::WRITE_PROTECTED) ? true: false);
    if ((grallocUsage & am_gralloc_get_video_decoder_full_buffer_usage())
        == am_gralloc_get_video_decoder_full_buffer_usage()) {
        halUsage = FULL_BUFFER_USAGE;
    } else if ((grallocUsage & am_gralloc_get_video_decoder_one_sixteenth_buffer_usage())
        == am_gralloc_get_video_decoder_one_sixteenth_buffer_usage()) {
        halUsage = ONE_SIXTEENTH_BUFFER_USAGE;
    }

    int ret = mVideoDecWraper->allocTunnelBuffer(halUsage, format, width, width, height, secure, sharefd);
    if (ret < 0) {
        return C2_BAD_VALUE;
    }
    return C2_OK;
}

c2_status_t TunnelBufferUtil::freeTunnelBuffer(int fd) {
    DCHECK(mVideoDecWraper != NULL);
    if (fd >= 0) {
        mVideoDecWraper->freeTunnelBuffer(fd);
    }
    return C2_OK;
}

}
