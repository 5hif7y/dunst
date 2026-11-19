/* SPDX-License-Identifier: BSD-3-Clause */
/* Subset of wordexp(3) dunst needs: ~, $VAR, ${VAR}, and %VAR% (Windows).
 * No command substitution (dunst passes WRDE_NOCMD anyway) and no field
 * splitting: dunst joins the words with " " right after. */
#ifndef DUNST_COMPAT_WORDEXP_H
#define DUNST_COMPAT_WORDEXP_H
#include <stddef.h>
typedef struct { size_t we_wordc; char **we_wordv; size_t we_offs; } wordexp_t;
enum { WRDE_NOCMD = 1 << 2, WRDE_UNDEF = 1 << 5 };
enum { WRDE_BADCHAR = 1, WRDE_BADVAL, WRDE_CMDSUB, WRDE_NOSPACE, WRDE_SYNTAX };
int wordexp(const char *words, wordexp_t *we, int flags);
void wordfree(wordexp_t *we);
#endif
