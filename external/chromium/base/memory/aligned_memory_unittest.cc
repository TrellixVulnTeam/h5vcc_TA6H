// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/aligned_memory.h"
#include "base/memory/scoped_ptr.h"
#include "testing/gtest/include/gtest/gtest.h"

#define EXPECT_ALIGNED(ptr, align) \
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(ptr) & (align - 1))

namespace {

using base::AlignedMemory;

TEST(AlignedMemoryTest, StaticAlignment) {
  static AlignedMemory<8, 8> raw8;
  static AlignedMemory<8, 16> raw16;
  static AlignedMemory<8, 256> raw256;
  static AlignedMemory<8, 4096> raw4096;

  EXPECT_EQ(8u, ALIGNOF(raw8));
  EXPECT_EQ(16u, ALIGNOF(raw16));
#if !defined(__LB_XB360__)
  // On the Xbox360, AlignedMemory is not aligned, but its data pointer is.
  EXPECT_EQ(256u, ALIGNOF(raw256));
  EXPECT_EQ(4096u, ALIGNOF(raw4096));
#endif

  EXPECT_ALIGNED(raw8.void_data(), 8);
  EXPECT_ALIGNED(raw16.void_data(), 16);
  EXPECT_ALIGNED(raw256.void_data(), 256);
  EXPECT_ALIGNED(raw4096.void_data(), 4096);
}

TEST(AlignedMemoryTest, StackAlignment) {
  AlignedMemory<8, 8> raw8;
  AlignedMemory<8, 16> raw16;
  AlignedMemory<8, 256> raw256;

  EXPECT_EQ(8u, ALIGNOF(raw8));
  EXPECT_EQ(16u, ALIGNOF(raw16));
#if !defined(__LB_XB360__)
  // On the Xbox360, AlignedMemory is not aligned, but its data pointer is.
  EXPECT_EQ(256u, ALIGNOF(raw256));
#endif

  EXPECT_ALIGNED(raw8.void_data(), 8);
  EXPECT_ALIGNED(raw16.void_data(), 16);
  EXPECT_ALIGNED(raw256.void_data(), 256);

  // TODO: This test hits an armv7 bug in clang. crbug.com/138066
#if !defined(ARCH_CPU_ARM_FAMILY)
  AlignedMemory<8, 4096> raw4096;
#if !defined(__LB_XB360__)
  // On the Xbox360, AlignedMemory is not aligned, but its data pointer is.
  EXPECT_EQ(4096u, ALIGNOF(raw4096));
#endif
  EXPECT_ALIGNED(raw4096.void_data(), 4096);
#endif  // !(defined(OS_IOS) && defined(ARCH_CPU_ARM_FAMILY))
}

TEST(AlignedMemoryTest, DynamicAllocation) {
  void* p = base::AlignedAlloc(8, 8);
  EXPECT_TRUE(p);
  EXPECT_ALIGNED(p, 8);
  base::AlignedFree(p);

  p = base::AlignedAlloc(8, 16);
  EXPECT_TRUE(p);
  EXPECT_ALIGNED(p, 16);
  base::AlignedFree(p);

  p = base::AlignedAlloc(8, 256);
  EXPECT_TRUE(p);
  EXPECT_ALIGNED(p, 256);
  base::AlignedFree(p);

  p = base::AlignedAlloc(8, 4096);
  EXPECT_TRUE(p);
  EXPECT_ALIGNED(p, 4096);
  base::AlignedFree(p);
}

TEST(AlignedMemoryTest, ScopedDynamicAllocation) {
  scoped_ptr_malloc<float, base::ScopedPtrAlignedFree> p(
      static_cast<float*>(base::AlignedAlloc(8, 8)));
  EXPECT_TRUE(p.get());
  EXPECT_ALIGNED(p.get(), 8);
}

}  // namespace
