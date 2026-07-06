/*
** gluos.h - operating system dependencies for GLU
**
*/
#ifdef __VMS
#ifdef __cplusplus
#pragma message disable nocordel
#pragma message disable codeunreachable
#pragma message disable codcauunr
#endif
#endif

#ifdef __WATCOMC__
/* Disable *lots* of warnings to get a clean build. I can't be bothered fixing the
 * code at the moment, as it is pretty ugly.
 */
#define GLU_WATCOM_WARNING_LEVEL 10
#pragma warning 7   GLU_WATCOM_WARNING_LEVEL
#pragma warning 13  GLU_WATCOM_WARNING_LEVEL
#pragma warning 14  GLU_WATCOM_WARNING_LEVEL
#pragma warning 367 GLU_WATCOM_WARNING_LEVEL
#pragma warning 379 GLU_WATCOM_WARNING_LEVEL
#pragma warning 726 GLU_WATCOM_WARNING_LEVEL
#pragma warning 836 GLU_WATCOM_WARNING_LEVEL
#endif

#ifdef BUILD_FOR_SNAP

#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>

#elif defined(_WIN32)

#include <stdlib.h>	    /* For _MAX_PATH definition */
#include <stdio.h>
#include <malloc.h>

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOIME
#define NOMINMAX

#ifdef __MINGW64_VERSION_MAJOR
  #undef _WIN32_WINNT
#endif

#ifndef _WIN32_WINNT
  /* Work around a mingw-w64 header bug triggered by NOGDI when
   * _WIN32_WINNT >= 0x0600. */
  #define _WIN32_WINNT 0x0400
#endif
#ifndef STRICT
  #define STRICT 1
#endif

#include <windows.h>

/* Disable warnings */
#if defined(_MSC_VER)
#pragma warning(disable : 4101)
#pragma warning(disable : 4244)
#pragma warning(disable : 4761)
#endif

#if defined(_MSC_VER) && _MSC_VER >= 1200 && _MSC_VER < 1300
#pragma comment(linker, "/OPT:NOWIN98")
#endif

#ifndef WINGDIAPI
#define WINGDIAPI
#endif

#elif defined(__OS2__)

#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#define WINGDIAPI

#else

/* Disable Microsoft-specific keywords */
#define GLAPIENTRY
#define WINGDIAPI

#endif
