/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef DUNST_COMPAT_PWD_H
#define DUNST_COMPAT_PWD_H
#include <glib.h>
struct passwd { char *pw_dir; };
/* /etc/passwd has no Windows equivalent; the profile directory is the home */
static inline struct passwd *getpwuid(int uid)
{
        static struct passwd pw;
        (void)uid;
        pw.pw_dir = (char *)g_get_home_dir();
        return &pw;
}
#endif
