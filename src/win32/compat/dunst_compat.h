/* SPDX-License-Identifier: BSD-3-Clause */
/* Force-included (/FI) into every dunst translation unit on Windows.
 * Same technique as the OpenBSD coreutils port (porter guide, module 04). */
#ifndef DUNST_WIN32_COMPAT_H
#define DUNST_WIN32_COMPAT_H
#ifdef _WIN32

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_NONSTDC_NO_DEPRECATE
#endif
#define NOMINMAX
#define _USE_MATH_DEFINES /* M_PI in draw.c */
#include <stdio.h>
#include <stddef.h>
#include <BaseTsd.h>
#include <string.h>

typedef SSIZE_T ssize_t;

/* <sys/stat.h> file type bits POSIX names (MSVC only has the _S_ ones) */
#ifndef S_IFIFO
#define S_IFIFO 0x1000 /* _S_IFIFO */
#endif
#ifndef S_IFREG
#define S_IFREG 0x8000 /* _S_IFREG */
#endif

#define strcasecmp _stricmp
#define strncasecmp _strnicmp

/* POSIX getline(3); used by ini.c */
ssize_t getline(char **lineptr, size_t *n, FILE *stream);

#endif
#endif
