/* SPDX-License-Identifier: BSD-3-Clause */
/* scandir(3)/alphasort(3) on GDir (UTF-8 names). settings.c only reads
 * the drop-in directory dunstrc.d with them. */
#ifndef DUNST_COMPAT_DIRENT_H
#define DUNST_COMPAT_DIRENT_H
struct dirent { char d_name[1024]; };
int scandir(const char *dir, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **));
int alphasort(const struct dirent **a, const struct dirent **b);
#endif
