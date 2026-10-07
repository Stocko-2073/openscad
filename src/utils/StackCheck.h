#pragma once

#include <cstdlib>

#include "platform/PlatformUtils.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 26486)  // Disable warning for dangling pointers
#endif                            // defined(_MSC_VER)

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
  // Per thread: animation prefetch workers must measure against their own stack.
  static inline thread_local unsigned char *ptr = nullptr;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif  // defined(_MSC_VER)
