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

#ifndef ANDROID_VIDEO_DECODE_ACCELERATOR_ADAPTOR_H
#define ANDROID_VIDEO_DECODE_ACCELERATOR_ADAPTOR_H

#include <rect.h>
#include <size.h>
#include <video_codecs.h>
#include <video_pixel_format.h>

#include <vector>

#include <inttypes.h>

enum class HalPixelFormat : uint32_t {
    UNKNOWN = 0x0,
    YCRCB_420_SP = 0x11,
    YCbCr_420_888 = 0x23,
    YV12 = 0x32315659,
    NV12 = 0x3231564e,
    NV21 = 0x3132564e,
};

// The offset and stride of a video frame plane.
struct VideoFramePlane {
    intptr_t mAddr;
    uint64_t mSize;
    uint32_t mOffset;
    uint32_t mStride;
};

// The HAL pixel format information supported by Android flexible YUV format.
struct SupportedPixelFormat {
    bool mCrcb;
    bool mSemiplanar;
    HalPixelFormat mPixelFormat;
};

// Video decoder accelerator adaptor interface.
// The adaptor plays the role of providing unified adaptor API functions and client callback to
// codec component side.
// The adaptor API and client callback are modeled after media::VideoDecodeAccelerator which is
// ported from Chrome and are 1:1 mapped with its API functions.
class VideoDecodeAcceleratorAdaptor {
public:
    enum Result {
        SUCCESS = 0,
        ILLEGAL_STATE = 1,
        INVALID_ARGUMENT = 2,
        UNREADABLE_INPUT = 3,
        PLATFORM_FAILURE = 4,
        INSUFFICIENT_RESOURCES = 5,
    };

    // The adaptor client interface. This interface should be implemented in the component side.
    class Client {
    public:
        virtual ~Client() {}

        // Callback to tell client how many and what size of buffers to provide.
        virtual void providePictureBuffers(uint32_t minNumBuffers,
                                           const media::Size& codedSize) = 0;

        // Callback to dismiss picture buffer that was assigned earlier.
        virtual void dismissPictureBuffer(int32_t pictureBufferId) = 0;

        // Callback to deliver decoded pictures ready to be displayed.
        virtual void pictureReady(int32_t pictureBufferId, int32_t bitstreamId,
                                  const media::Rect& cropRect) = 0;

        virtual void userdataReady(const std::vector<uint8_t>& userdata) = 0;

        virtual void updateDecInfo(const std::vector<uint8_t>& decinfo) = 0;

        // Callback to notify that decoder has decoded the end of the bitstream buffer with
        // specified ID.
        virtual void notifyEndOfBitstreamBuffer(int32_t bitstreamId) = 0;

        // Flush completion callback.
        virtual void notifyFlushDone() = 0;

        // Reset completion callback.
        virtual void notifyResetDone() = 0;

        // Callback to notify about errors. Note that errors in initialize() will not be reported
        // here, instead of by its returned value.
        virtual void notifyError(Result error) = 0;
        virtual void notifyEvent(uint32_t event, void* param, uint32_t paramsize) = 0;
    };

    // Initializes the video decoder with specific profile. This call is synchronous and returns
    // SUCCESS iff initialization is successful.
    virtual Result initialize(media::VideoCodecProfile profile, bool secureMode,
                    VideoDecodeAcceleratorAdaptor::Client* client,
                    const std::vector<uint8_t>& param,
                    uint32_t instanceNum = 0) = 0;

    // Decodes given buffer handle with bitstream ID.
    virtual void decode(int32_t bitstreamId, int handleFd, off_t offset, uint32_t bytesUsed, uint64_t timestamp) = 0;

    // Decodes given buffer handle with bitstream ID.
    virtual void decode(int32_t bitstreamId, uint8_t* pbuf, off_t offset, uint32_t bytesUsed, uint64_t timestamp) = 0;

    // Assigns a specified number of picture buffer set to the video decoder.
    virtual void assignPictureBuffers(uint32_t numOutputBuffers) = 0;

    // Imports planes as backing memory for picture buffer with specified ID.
    virtual void importBufferForPicture(int32_t pictureBufferId, HalPixelFormat format,
                                        int handleFd,
                                        int metaFd,
                                        const std::vector<VideoFramePlane>& planes) = 0;

    // Sends picture buffer to be reused by the decoder by its picture ID.
    virtual void reusePictureBuffer(int32_t pictureBufferId) = 0;

    // Flushes the decoder.
    virtual void flush() = 0;

    // Resets the decoder.
    virtual void reset() = 0;

    // Destroys the decoder.
    virtual void destroy() = 0;

    virtual int32_t sendCommand(uint32_t index, void* param, uint32_t size) = 0;

    virtual ~VideoDecodeAcceleratorAdaptor() {}
};

#endif  // ANDROID_VIDEO_DECODE_ACCELERATOR_ADAPTOR_H
