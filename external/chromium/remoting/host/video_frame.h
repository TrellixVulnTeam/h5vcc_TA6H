// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef REMOTING_HOST_VIDEO_FRAME_H_
#define REMOTING_HOST_VIDEO_FRAME_H_

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/memory/ref_counted.h"
#include "remoting/base/shared_buffer.h"
#include "third_party/skia/include/core/SkTypes.h"
#include "third_party/skia/include/core/SkSize.h"

namespace remoting {

// Represents a video frame.
class VideoFrame {
 public:
  virtual ~VideoFrame();

  int bytes_per_row() const { return bytes_per_row_; }
  const SkISize& dimensions() const { return dimensions_; }
  uint8* pixels() const { return pixels_; }
  const scoped_refptr<SharedBuffer>& shared_buffer() const {
    return shared_buffer_;
  }

 protected:
  // Initializes an empty video frame. Derived classes are expected to allocate
  // memory for the frame in a platform-specific way and set the properties of
  // the allocated frame.
  VideoFrame();

  void set_bytes_per_row(int bytes_per_row) {
    bytes_per_row_ = bytes_per_row;
  }

  void set_dimensions(const SkISize& dimensions) { dimensions_ = dimensions; }
  void set_pixels(uint8* ptr) { pixels_ = ptr; }
  void set_shared_buffer(scoped_refptr<SharedBuffer> shared_buffer) {
    shared_buffer_ = shared_buffer;
  }

 private:
  // Bytes per row of pixels including necessary padding.
  int bytes_per_row_;

  // Dimensions of the buffer in pixels.
  SkISize dimensions_;

  // Points to the pixel buffer.
  uint8* pixels_;

  // Points to an optional shared memory buffer that backs up |pixels_| buffer.
  scoped_refptr<SharedBuffer> shared_buffer_;

  DISALLOW_COPY_AND_ASSIGN(VideoFrame);
};

}  // namespace remoting

#endif  // REMOTING_HOST_VIDEO_FRAME_H_
