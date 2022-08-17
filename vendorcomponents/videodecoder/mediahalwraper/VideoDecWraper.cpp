/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */

#define LOG_NDEBUG 0
#define LOG_TAG "VideoDecWraper"
#include <utils/Log.h>
#include <VideoDecWraper.h>
#include <video_decode_accelerator.h>
#include <dlfcn.h>
#include <unistd.h>

namespace android {

void* gMediaHal = NULL;

/*static*/
uint32_t VideoDecWraper::gInstanceNum = 0;
uint32_t VideoDecWraper::gInstanceCnt = 0;

#define VDEC_LOGV(format, ...) ALOGV("[%d ## %d]%s:%d >" format, mInstanceCnt, gInstanceNum, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define VDEC_LOGW(format, ...) ALOGW("[%d ## %d]%s:%d >" format, mInstanceCnt, gInstanceNum, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define VDEC_LOGD(format, ...) ALOGD("[%d ## %d]%s:%d >" format, mInstanceCnt, gInstanceNum, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define VDEC_LOGI(format, ...) ALOGI("[%d ## %d]%s:%d >" format, mInstanceCnt, gInstanceNum, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define VDEC_LOGE(format, ...) ALOGE("[%d ## %d]%s:%d >" format, mInstanceCnt, gInstanceNum, __FUNCTION__, __LINE__, ##__VA_ARGS__)

bool VideoDecWraper::loadMediaHalLibrary(void) {
    if (!gMediaHal) {
        gMediaHal = dlopen("libmediahal_videodec.so", RTLD_NOW);
        if (gMediaHal == NULL) {
            ALOGE("Unable to dlopen libmediahal_videodec: %s", dlerror());
            return false;
        }
    }
    return true;
}

static AmVideoDecBase* getAmVideoDec(AmVideoDecCallback* callback) {
    //default version is 1.0
    uint32_t versionM = 1;
    uint32_t versionL = 0;

    if (!VideoDecWraper::loadMediaHalLibrary())
        return NULL;

    typedef AmVideoDecBase *(*createAmVideoDecFunc)(AmVideoDecCallback* callback);
    typedef uint32_t (*getVersionFunc)(uint32_t* versionM, uint32_t* versionL);

    getVersionFunc getVersion = (getVersionFunc)dlsym(gMediaHal, "AmVideoDec_getVersion");
    if (getVersion != NULL)
        (*getVersion)(&versionM, &versionL);

    createAmVideoDecFunc getAmVideoDec = NULL;

    if ((versionM == 1) && (versionL == 0)) {
        ALOGE("version 1.0 use create AmMediaHal\n");
        getAmVideoDec =
            (createAmVideoDecFunc)dlsym(gMediaHal, "createAmMediaHal");
    } else if ((versionM == 1) && (versionL == 1)){
        ALOGE("version 1.1 use create AmVideoDec_create\n");
        getAmVideoDec =
            (createAmVideoDecFunc)dlsym(gMediaHal, "AmVideoDec_create");
    } else {
        ALOGE("Mediahal version do not right\n");
        dlclose(gMediaHal);
        gMediaHal = NULL;
        return NULL;
    }

    if (getAmVideoDec == NULL) {
        dlclose(gMediaHal);
        gMediaHal = NULL;
        ALOGE("Can not create AmVideoDec_create\n");
        return NULL;
    }

    AmVideoDecBase* halHandle = (*getAmVideoDec)(callback);
    ALOGI("GetAmVideoDec ok\n");
    return halHandle;
}

media::VideoDecodeAccelerator::SupportedProfiles VideoDecWraper::AmVideoDec_getSupportedProfiles(uint32_t inputcodec) {
    if (!VideoDecWraper::loadMediaHalLibrary()) {
        media::VideoDecodeAccelerator::SupportedProfiles supportedProfiles;
        return supportedProfiles;
    }

    typedef void (*fGetSupportedProfiles)(uint32_t inputcodec, uint32_t** data, uint32_t* size);
    fGetSupportedProfiles getSupportedProfiles = (fGetSupportedProfiles)dlsym(gMediaHal, "AmVideoDec_getSupportedProfiles");

    media::VideoDecodeAccelerator::SupportedProfile* pdata = NULL;
    uint arraysize = 0;

    getSupportedProfiles(inputcodec, (uint32_t**)&pdata, &arraysize);
    ALOGI("AmVideoDec_getSupportedProfiles data:%p, size:%d", pdata, arraysize);
    media::VideoDecodeAccelerator::SupportedProfiles supportedProfiles(pdata, pdata + arraysize);
    return supportedProfiles;
}

uint32_t VideoDecWraper::AmVideoDec_getResolveBufferFormat(bool crcb, bool semiplanar) {
    if (!VideoDecWraper::loadMediaHalLibrary())
        return 0;

    typedef uint32_t (*fGetResolveBufferFormat)(bool crcb, bool semiplanar);
    fGetResolveBufferFormat getResolveBufferFormat = (fGetResolveBufferFormat)dlsym(gMediaHal, "AmVideoDec_getResolveBufferFormat");
    ALOGD("AmVideoDec_getResolveBufferFormat");
    return getResolveBufferFormat(crcb, semiplanar);
}

VideoDecWraper::VideoDecWraper() :
    mAmVideoDec(NULL),
    mDecoderCallback(NULL) {
    gInstanceCnt++;
    gInstanceNum++;
    mInstanceCnt = gInstanceCnt;
    VDEC_LOGD("VideoDecWraper");
}

VideoDecWraper::~VideoDecWraper() {
    VDEC_LOGD("~VideoDecWraper");
    if (mAmVideoDec) {
        delete mAmVideoDec;
        mAmVideoDec = NULL;
    }
    gInstanceNum--;
}

int VideoDecWraper::initialize(
    const char* mime,
    uint8_t* config,
    uint32_t configLen,
    bool secureMode,
    VideoDecWraperCallback* client,
    int32_t flags) {
    VDEC_LOGD("initialize:mime:%s secureMode is %d flags is 0x%x", mime, secureMode, flags);
    if (!mAmVideoDec)
        mAmVideoDec = getAmVideoDec(this);
    if (!mAmVideoDec) {
        VDEC_LOGE("can not get AmVideoDec, init error\n");
        return -1;
    }
    int ret = mAmVideoDec->initialize(mime, config, configLen, secureMode, true, flags);
    if (ret != 0)
        return -1;
    mAmVideoDec->setQueueCount(64);
    if (client)
        mDecoderCallback = client;
    return 0;
}

int32_t VideoDecWraper::decode(
    int32_t bitstreamId,
    uint8_t* pbuf,
    off_t offset,
    uint32_t bytesUsed,
    uint64_t timestamp,
    uint8_t* hdrbuf,
    uint32_t hdrlen,
    int32_t flags) {
    VDEC_LOGD("decode bitstreamId:%d bytesUsed :%d timestamp:%lld flags is 0x%x",
        bitstreamId, bytesUsed, timestamp, flags);
    if (mAmVideoDec) {
        if (hdrlen > 0)
            return mAmVideoDec->queueInputBuffer(bitstreamId,pbuf, offset, bytesUsed, timestamp, hdrbuf, hdrlen,flags);
        else
            return mAmVideoDec->queueInputBuffer(bitstreamId, pbuf, offset, bytesUsed,timestamp, flags);
    }
    return -1;
}

int32_t VideoDecWraper::decode(
    int32_t bitstreamId,
    int ashmemFd,
    off_t offset,
    uint32_t bytesUsed,
    uint64_t timestamp,
    uint8_t* hdrbuf,
    uint32_t hdrlen,
    int32_t flags) {
    VDEC_LOGD("decode bitstreamId:%d bytesUsed :%d timestamp:%lld flags is 0x%x",
        bitstreamId, bytesUsed, timestamp, flags);
    if (mAmVideoDec) {
        if (hdrlen > 0)
            return mAmVideoDec->queueInputBuffer(bitstreamId, ashmemFd, offset,bytesUsed, timestamp, hdrbuf, hdrlen,flags);
        else
            return mAmVideoDec->queueInputBuffer(bitstreamId, ashmemFd, offset,bytesUsed, timestamp, flags);
    }
    return -1;
}

void VideoDecWraper::assignPictureBuffers(uint32_t numOutputBuffers) {
    VDEC_LOGD("assignPictureBuffers:%d", numOutputBuffers);
    if (mAmVideoDec)
        mAmVideoDec->setupOutputBufferNum(numOutputBuffers);
}

void VideoDecWraper::importBufferForPicture(
    int32_t pictureBufferId,
    int fd,
    int metafd,
    uint8_t* buf,
    size_t size,
    bool isNV21) {
    VDEC_LOGD("importBufferForPicture:%d, fd:%d, metafd:%d", pictureBufferId, fd, metafd);
    if (mAmVideoDec) {
        VDEC_LOGD("outbuf color format %s", isNV21? "NV21" : "NV12");
        mAmVideoDec->createOutputBuffer(pictureBufferId,
                dup(fd), isNV21, metafd >= 0 ? dup(metafd) : -1);
    }
}

void VideoDecWraper::reusePictureBuffer(int32_t pictureBufferId) {
    VDEC_LOGD("reusePictureBuffer pictureBufferId:%d", pictureBufferId);
    if (mAmVideoDec)
        mAmVideoDec->queueOutputBuffer(pictureBufferId);
}

int32_t VideoDecWraper::allocTunnelBuffer(int usage, uint32_t format, int stride, uint32_t width, uint32_t height, bool secure, int* fd) {
    if (mAmVideoDec)
        return mAmVideoDec->allocTunnelBuffer(usage, format, stride, width, height, secure, fd);
    return -1;
}

int32_t VideoDecWraper::freeTunnelBuffer(int fd) {
    if (mAmVideoDec)
        return mAmVideoDec->freeTunnelBuffer(fd);
    return -1;
}

void VideoDecWraper::eosFlush() {
    VDEC_LOGD("eosFlush");
    if (mAmVideoDec)
        mAmVideoDec->flush();
}

void VideoDecWraper::reset(uint32_t flags) {
    VDEC_LOGD("reset");
    if (mAmVideoDec)
        mAmVideoDec->reset(flags);
}

void VideoDecWraper::flush(uint32_t flags) {
    VDEC_LOGD("flush");
    if (mAmVideoDec)
        mAmVideoDec->reset(flags);
}

void VideoDecWraper::stop(uint32_t flags) {
    VDEC_LOGD("stop");
    if (mAmVideoDec)
        mAmVideoDec->reset(flags);
}

void VideoDecWraper::destroy() {
    VDEC_LOGD("destroy");
    if (mAmVideoDec)
        mAmVideoDec->destroy();
}

// callback
void VideoDecWraper::onOutputFormatChanged(uint32_t requested_num_of_buffers,
            int32_t width, uint32_t height) {
    VDEC_LOGD("providePictureBuffers:minNumBuffers:%d, w:%d, h:%d", requested_num_of_buffers, width, height);
    if (mDecoderCallback)
        mDecoderCallback->ProvidePictureBuffers(requested_num_of_buffers, width, height);
}

void VideoDecWraper::onOutputBufferDone(int32_t pictureBufferId, int64_t bitstreamId,
            uint32_t width, uint32_t height) {
    VDEC_LOGD("pictureReady:pictureBufferId:%d bitstreamId:%lld mDecoderCallback:%p w:%d, h:%d",
        pictureBufferId, bitstreamId, mDecoderCallback,width, height);
    if (mDecoderCallback)
        mDecoderCallback->PictureReady(pictureBufferId, bitstreamId, 0, 0, width, height, 0);
}

void VideoDecWraper::onOutputBufferDone(int32_t pictureBufferId, int64_t bitstreamId,
            uint32_t width, uint32_t height, int32_t flags) {
    (void)flags;
    VDEC_LOGD("pictureReady:pictureBufferId:%d bitstreamId:%lld mDecoderCallback:%p w:%d, h:%d, flags:%d",
        pictureBufferId, bitstreamId, mDecoderCallback, width, height, flags);
    if (mDecoderCallback)
        mDecoderCallback->PictureReady(pictureBufferId, bitstreamId, 0, 0, width, height,flags);
}

void VideoDecWraper::onInputBufferDone(int32_t bitstream_buffer_id) {
    VDEC_LOGD("notifyEndOfBitstreamBuffer:bitstream_buffer_id:%d", bitstream_buffer_id);
    if (mDecoderCallback)
        mDecoderCallback->NotifyEndOfBitstreamBuffer(bitstream_buffer_id);
}

void VideoDecWraper::onUserdataReady(const uint8_t* userdata, uint32_t usize) {
    VDEC_LOGD("onUserdataReady %p, size %d\n", userdata, usize);
}

void VideoDecWraper::onUpdateDecInfo(const uint8_t* info, uint32_t isize) {
    VDEC_LOGD("onUpdateDecInfo info %p, size %d\n", info, isize);
    if (mDecoderCallback)
        mDecoderCallback->UpdateDecInfo(info, isize);
}

void VideoDecWraper::onFlushDone() {
    VDEC_LOGD("onFlushDone\n");
    if (mDecoderCallback)
        mDecoderCallback->NotifyFlushDone();
}

void VideoDecWraper::onResetDone() {
    VDEC_LOGD("onResetDone\n");
    if (mDecoderCallback)
        mDecoderCallback->NotifyFlushOrStopDone();
}

void VideoDecWraper::onError(int32_t error) {
    VDEC_LOGD("notifyError:%d", error);
    if (mDecoderCallback)
        mDecoderCallback->NotifyError((int)error);
}

void VideoDecWraper::onEvent(uint32_t event, void* param, uint32_t paramsize) {
    VDEC_LOGD("event %d, param %p, paramsize:%d\n", event, param, paramsize);
    if (mDecoderCallback)
        mDecoderCallback->NotifyEvent(event, param, paramsize);
}

}



