/* nvfmt.h - portable, always-bounded string formatting.
 *
 * MSVC deprecates the `_snprintf` / `_snwprintf` family (C4996), and its
 * secure replacements `_snprintf_s` / `_snwprintf_s` do not exist in
 * MinGW's CRT. These macros pick the bounded, always-NUL-terminating
 * variant available on whichever compiler is building, so the same source
 * compiles warning-free at /W4 /WX on MSVC and clean under -Wall on MinGW.
 *
 * The third branch exists because src/values.c is compiled by the host-side
 * test suite on Linux. glibc removed `snwprintf` back in 2.2 - the C99 wide
 * entry point is `swprintf`, which has the same (dst, count, fmt, ...)
 * argument shape as MinGW's. Linking values.c against glibc without this
 * failed with "undefined reference to `snwprintf`".
 */
#ifndef NVFMT_H
#define NVFMT_H

#if defined(_MSC_VER)
  /* _TRUNCATE means "write at most size-1 chars plus a NUL". */
  #define NV_SNPRINTF(dst, cnt, ...)  _snprintf_s((dst), (size_t)(cnt), _TRUNCATE, __VA_ARGS__)
  #define NV_SNWPRINTF(dst, cnt, ...) _snwprintf_s((dst), (size_t)(cnt), _TRUNCATE, __VA_ARGS__)
#elif defined(__MINGW32__) || defined(__MINGW64__)
  #define NV_SNPRINTF(dst, cnt, ...)  snprintf((dst), (size_t)(cnt), __VA_ARGS__)
  #define NV_SNWPRINTF(dst, cnt, ...) snwprintf((dst), (size_t)(cnt), __VA_ARGS__)
#else
  /* glibc / POSIX: swprintf, not snwprintf. */
  #define NV_SNPRINTF(dst, cnt, ...)  snprintf((dst), (size_t)(cnt), __VA_ARGS__)
  #define NV_SNWPRINTF(dst, cnt, ...) swprintf((dst), (size_t)(cnt), __VA_ARGS__)
#endif

#endif /* NVFMT_H */
