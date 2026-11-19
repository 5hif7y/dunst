/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef DUNST_COMPAT_UNISTD_H
#define DUNST_COMPAT_UNISTD_H
#include <io.h>
#include <process.h>
#include <direct.h>
#define R_OK 4
#define W_OK 2
#define X_OK 0 /* no execute bit on Windows: existence is the best we have */
#define F_OK 0
static inline int getuid(void) { return 0; }
#endif
