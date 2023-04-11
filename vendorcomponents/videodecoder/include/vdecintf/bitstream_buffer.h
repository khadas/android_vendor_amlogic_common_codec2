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

#ifndef MEDIA_BASE_BITSTREAM_BUFFER_H_
#define MEDIA_BASE_BITSTREAM_BUFFER_H_

#include <stddef.h>
#include <stdint.h>

#include "base/macros.h"
#include "base/memory/shared_memory.h"
#include "base/time/time.h"

namespace media {

// Indicates an invalid or missing timestamp.
constexpr base::TimeDelta kNoTimestamp =
    base::TimeDelta::FromMicroseconds(std::numeric_limits<int64_t>::min());

// Class for passing bitstream buffers around.  Does not take ownership of the
// data.  This is the media-namespace equivalent of PP_VideoBitstreamBuffer_Dev.
class BitstreamBuffer {
 public:
  BitstreamBuffer();

  // Constructs a new BitstreamBuffer. The content of the bitstream is located
  // at |offset| bytes away from the start of the shared memory and the payload
  // is |size| bytes. When not provided, the default value for |offset| is 0.
  // |presentation_timestamp| is when the decoded frame should be displayed.
  // When not provided, |presentation_timestamp| will be
  // |media::kNoTimestamp|.
  BitstreamBuffer(int32_t id,
                  base::SharedMemoryHandle handle,
                  size_t size,
                  off_t offset = 0,
                  base::TimeDelta presentation_timestamp = kNoTimestamp);
  BitstreamBuffer(int32_t id,
                  uint8_t* pbuf,
                  size_t size,
                  off_t offset = 0,
                  base::TimeDelta presentation_timestamp = kNoTimestamp);

  BitstreamBuffer(const BitstreamBuffer& other);

  ~BitstreamBuffer();

  int32_t id() const { return id_; }
  base::SharedMemoryHandle handle() const { return handle_; }

  // The number of bytes of the actual bitstream data. It is the size of the
  // content instead of the whole shared memory.
  size_t size() const { return size_; }

  // The offset to the start of actual bitstream data in the shared memory.
  off_t offset() const { return offset_; }

  // The timestamp is only valid if it's not equal to |media::kNoTimestamp|.
  base::TimeDelta presentation_timestamp() const {
    return presentation_timestamp_;
  }

  void set_handle(const base::SharedMemoryHandle& handle) { handle_ = handle; }

  uint8_t* get_buf() const { return pbuf_;}

 private:
  int32_t id_;
  base::SharedMemoryHandle handle_;
  uint8_t* pbuf_;
  size_t size_;
  off_t offset_;

  // This is only set when necessary. For example, AndroidVideoDecodeAccelerator
  // needs the timestamp because the underlying decoder may require it to
  // determine the output order.
  base::TimeDelta presentation_timestamp_;

  // Allow compiler-generated copy & assign constructors.
};

}  // namespace media

#endif  // MEDIA_BASE_BITSTREAM_BUFFER_H_
