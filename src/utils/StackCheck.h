#pragma once

#include <cstdlib>

#include "platform/PlatformUtils.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 26486)  // Disable warning for dangling pointers
#endif                            // defined(_MSC_VER)

// Tracks each thread's stack growth from a per-thread anchor. The anchor
// (`ptr`) is taken from a local variable on first `check()` for the current
// thread, so recursion depth is measured relative to the calling thread's
// stack — not the main thread's. Without this, workers (animation pre-fetch)
// would compare addresses on a foreign stack and trip the recursion guard
// immediately.
class StackCheck
{
public:
  static StackCheck& inst()
  {
    static StackCheck instance;
    return instance;
  }

  inline bool check()
  {
    unsigned char c;
    if (ptr == nullptr) {
      ptr = &c;  // NOLINT(*StackAddressEscape)
      return false;
    }
    return static_cast<unsigned long>(std::abs(ptr - &c)) >= limit;
  }

private:
  StackCheck() : limit(PlatformUtils::stackLimit()) {}

  unsigned long limit;
  // Per-thread stack anchor: each thread sets its own on its first check().
  static inline thread_local unsigned char *ptr = nullptr;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif  // defined(_MSC_VER)
