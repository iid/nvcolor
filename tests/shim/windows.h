/* tests/shim/windows.h - just enough of windows.h to compile the pure maths
 * modules on a host that has no Windows SDK.
 *
 * Deliberately tiny. It grew by exactly what src/values.c needed and nothing
 * more: values.c formats wide strings (the readouts) and copies them into
 * caller-supplied buffers, which is what lstrcpynW is for. The real
 * declarations live in <wchar.h> and windows.h; defining them here keeps the
 * production source compiling unmodified.
 */
#ifndef FAKE_WINDOWS_H
#define FAKE_WINDOWS_H

#include <wchar.h>   /* wchar_t, wcscmp */

typedef unsigned short WORD;
typedef struct { int dummy; } *HDC;
#define TRUE 1
#define FALSE 0

int  GetDeviceGammaRamp(HDC hdc, void *lpRamp);
int  SetDeviceGammaRamp(HDC hdc, void *lpRamp);

/* Bounded wide-string copy. Real signature:
 *   int lstrcpynW(wchar_t *dst, const wchar_t *src, int cch);
 * Copies at most cch-1 characters and always NUL-terminates. */
int  lstrcpynW(wchar_t *dst, const wchar_t *src, int cch);

#endif
