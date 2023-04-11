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

#ifndef RECT_H_
#define RECT_H_

#include <string>

#include "base/strings/stringprintf.h"
#include "size.h"

namespace media {

// Helper struct for rect to replace gfx::Rect usage from original code.
// Only partial functions of gfx::Rect is implemented here.
class Rect {
 public:
  Rect() : x_(0), y_(0), size_(0, 0) {}
  Rect(int width, int height) : x_(0), y_(0), size_(width, height) {}
  Rect(int x, int y, int width, int height)
      : x_(x), y_(y), size_(width, height) {}
  explicit Rect(const Size& size) : x_(0), y_(0), size_(size) {}

  int x() const { return x_; }
  void set_x(int x) { x_ = x; }

  int y() const { return y_; }
  void set_y(int y) { y_ = y; }

  int width() const { return size_.width(); }
  void set_width(int width) { size_.set_width(width); }

  int height() const { return size_.height(); }
  void set_height(int height) { size_.set_height(height); }

  const Size& size() const { return size_; }
  void set_size(const Size& size) {
    set_width(size.width());
    set_height(size.height());
  }

  constexpr int right() const { return x() + width(); }
  constexpr int bottom() const { return y() + height(); }

  void SetRect(int x, int y, int width, int height) {
    set_x(x);
    set_y(y);
    set_width(width);
    set_height(height);
  }

  // Returns true if the area of the rectangle is zero.
  bool IsEmpty() const { return size_.IsEmpty(); }

  // Returns true if this rectangle contains the specified rectangle.
  bool Contains(const Rect& rect) const {
    return (rect.x() >= x() && rect.right() <= right() && rect.y() >= y() &&
            rect.bottom() <= bottom());
  }

  std::string ToString() const {
    return base::StringPrintf("(%d,%d) %s",
                              x_, y_, size().ToString().c_str());
  }

 private:
  int x_;
  int y_;
  Size size_;
};

inline bool operator==(const Rect& lhs, const Rect& rhs) {
  return lhs.x() == rhs.x() && lhs.y() == rhs.y() && lhs.size() == rhs.size();
}

inline bool operator!=(const Rect& lhs, const Rect& rhs) {
  return !(lhs == rhs);
}

}  // namespace media

#endif  // RECT_H_
