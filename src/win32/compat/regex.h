/* SPDX-License-Identifier: BSD-3-Clause */
/* POSIX <regex.h> on top of GLib's GRegex (PCRE2). menu.c and rules.c use
 * extended syntax, which PCRE accepts as a superset for their patterns. */
#ifndef DUNST_COMPAT_REGEX_H
#define DUNST_COMPAT_REGEX_H
#include <stddef.h>
typedef ptrdiff_t regoff_t;
typedef struct { void *re; size_t re_nsub; int nosub; } regex_t;
typedef struct { regoff_t rm_so, rm_eo; } regmatch_t;
#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NOSUB 4
#define REG_NEWLINE 8
#define REG_NOMATCH 1
#define REG_BADPAT 2
int regcomp(regex_t *preg, const char *regex, int cflags);
int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size);
void regfree(regex_t *preg);
#endif
