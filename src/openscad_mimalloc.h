#pragma once

#ifdef USE_MIMALLOC

#if defined(MI_OVERRIDE) || (defined(_WIN32) && defined(MI_LINK_STATIC))
  // MI_OVERRIDE builds already define operator new/delete; on Windows the override header
  // crashes when mimalloc is statically linked.
#include <mimalloc.h>
#else
  // Routes C++ new/delete through mimalloc, which would otherwise serve only GMP.
  // Include this header in exactly one translation unit.
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
