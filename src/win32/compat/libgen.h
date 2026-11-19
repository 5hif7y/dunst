/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef DUNST_COMPAT_LIBGEN_H
#define DUNST_COMPAT_LIBGEN_H
#include <string.h>
/* basename(3) modifying its argument, accepting both separators */
static inline char *basename(char *path)
{
        char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
        char *s = a > b ? a : b;
        return s ? s + 1 : path;
}
#endif
