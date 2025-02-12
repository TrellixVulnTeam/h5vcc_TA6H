// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/aligned_memory.h"

#include "base/logging.h"

#if defined(OS_ANDROID) || defined(OS_NACL) || defined(__LB_SHELL__)
#include <malloc.h>
#endif

namespace base {

void* AlignedAlloc(size_t size, size_t alignment) {
  DCHECK_GT(size, 0U);
  DCHECK_EQ(alignment & (alignment - 1), 0U);
  DCHECK_EQ(alignment % sizeof(void*), 0U);
  void* ptr = NULL;
#if defined(COMPILER_MSVC)
  ptr = _aligned_malloc(size, alignment);
// Both Android and NaCl technically support posix_memalign(), but do not expose
// it in the current version of the library headers used by Chrome.  Luckily,
// memalign() on both platforms returns pointers which can safely be used with
// free(), so we can use it instead.  Issues filed with each project for docs:
// http://code.google.com/p/android/issues/detail?id=35391
// http://code.google.com/p/chromium/issues/detail?id=138579
#elif defined(OS_ANDROID) || defined(OS_NACL) || defined(__LB_SHELL__)
  ptr = memalign(alignment, size);
#else
  if (posix_memalign(&ptr, alignment, size))
    ptr = NULL;
#endif
  // Since aligned allocations may fail for non-memory related reasons, force a
  // crash if we encounter a failed allocation; maintaining consistent behavior
  // with a normal allocation failure in Chrome.
  if (!ptr) {
    DLOG(ERROR) << "If you crashed here, your aligned allocation is incorrect: "
                << "size=" << size << ", alignment=" << alignment;
    CHECK(false);
  }
  // Sanity check alignment just to be safe.
  DCHECK_EQ(reinterpret_cast<uintptr_t>(ptr) & (alignment - 1), 0U);
  return ptr;
}

}  // namespace base
