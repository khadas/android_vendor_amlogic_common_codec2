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

#ifndef NATIVE_PIXMAP_HANDLE_H_
#define NATIVE_PIXMAP_HANDLE_H_

#include <vector>

#include "base/file_descriptor_posix.h"

namespace media {

// NativePixmapPlane is used to carry the plane related information for GBM
// buffer. More fields can be added if they are plane specific.
struct NativePixmapPlane {
  // This is the same value as DRM_FORMAT_MOD_INVALID, which is not a valid
  // modifier. We use this to indicate that layout information
  // (tiling/compression) if any will be communicated out of band.
  static constexpr uint64_t kNoModifier = 0x00ffffffffffffffULL;

  NativePixmapPlane();
  NativePixmapPlane(intptr_t addr,
                    int stride,
                    int offset,
                    uint64_t size,
                    uint64_t modifier = kNoModifier);
  NativePixmapPlane(const NativePixmapPlane& other);
  ~NativePixmapPlane();

  // the plane address.
  intptr_t addr;
  // The strides and offsets in bytes to be used when accessing the buffers via
  // a memory mapping. One per plane per entry.
  int stride;
  int offset;
  // Size in bytes of the plane.
  // This is necessary to map the buffers.
  uint64_t size;
  // The modifier is retrieved from GBM library and passed to EGL driver.
  // Generally it's platform specific, and we don't need to modify it in
  // Chromium code. Also one per plane per entry.
  uint64_t modifier;
};

struct NativePixmapHandle {
  NativePixmapHandle();
  NativePixmapHandle(const NativePixmapHandle& other);

  ~NativePixmapHandle();

  // File descriptors for the underlying memory objects (usually dmabufs).
  std::vector<base::FileDescriptor> fds;
  std::vector<NativePixmapPlane> planes;
};

}  // namespace media

#endif  // NATIVE_PIXMAP_HANDLE_H_
