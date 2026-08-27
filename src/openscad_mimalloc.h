#pragma once

#ifdef USE_MIMALLOC

#if defined(MI_OVERRIDE) || (defined(_WIN32) && defined(MI_LINK_STATIC))
  // MI_OVERRIDE builds already define operator new/delete inside mimalloc, so
  // including the override header here would be a duplicate definition.
  // On Windows, mimalloc doesn't support static override of malloc at all, and
  // the new/delete override crashes when it is statically linked.
#include <mimalloc.h>
#else
  // Replace the global operator new/delete so C++ allocation goes through
  // mimalloc. This matters most on macOS, where MI_OVERRIDE is forced OFF
  // (submodules/CMakeLists.txt) because dyld interposition is unreliable for a
  // statically linked main executable - without this, mimalloc is linked but
  // only ever serves GMP, and every std::string/vector/shared_ptr in the
  // evaluator still goes to the system allocator.
  // This header must be included in exactly one translation unit.
#include <mimalloc-new-delete.h>
#endif

#if defined(ENABLE_CGAL)
#include <cstddef>
// gmp requires function signature with extra oldsize parameters for some reason.
inline void *gmp_realloc(void *ptr, size_t /*oldsize*/, size_t newsize)
{
  return mi_realloc(ptr, newsize);
}
inline void gmp_free(void *ptr, size_t /*oldsize*/)
{
  mi_free(ptr);
}
#include <gmp.h>
inline void init_mimalloc()
{
  mp_set_memory_functions(mi_malloc, gmp_realloc, gmp_free);
}
#endif  // ENABLE_CGAL

#endif  // USE_MIMALLOC
