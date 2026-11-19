/* SPDX-License-Identifier: BSD-3-Clause */
/* Implementations behind src/win32/compat/*.h */
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "dunst_compat.h"
#include "dirent.h"
#include "fnmatch.h"
#include "regex.h"
#include "wordexp.h"

/* ---- getline ----------------------------------------------------------- */

ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
        if (!lineptr || !n || !stream) {
                errno = EINVAL;
                return -1;
        }
        size_t len = 0;
        int c;
        while ((c = fgetc(stream)) != EOF) {
                if (len + 2 > *n || !*lineptr) {
                        size_t cap = *n ? *n * 2 : 128;
                        char *p = realloc(*lineptr, cap);
                        if (!p)
                                return -1;
                        *lineptr = p;
                        *n = cap;
                }
                (*lineptr)[len++] = (char)c;
                if (c == '\n')
                        break;
        }
        if (len == 0)
                return -1;
        (*lineptr)[len] = 0;
        return (ssize_t)len;
}

/* ---- fnmatch (*, ?, [...], [!...], \, FNM_PATHNAME, FNM_PERIOD) ---------- */

static int is_sep(char c, int flags) { return (flags & FNM_PATHNAME) && (c == '/' || c == '\\'); }

static int fnm(const char *p, const char *s, int flags, int at_start)
{
        for (;; p++, s++) {
                char c = *p;
                if (c == 0)
                        return *s ? FNM_NOMATCH : 0;
                if ((flags & FNM_PERIOD) && at_start && *s == '.' && c != '.')
                        return FNM_NOMATCH;
                at_start = 0;
                switch (c) {
                case '?':
                        if (!*s || is_sep(*s, flags))
                                return FNM_NOMATCH;
                        break;
                case '*':
                        while (p[1] == '*')
                                p++;
                        for (const char *t = s;; t++) {
                                if (!fnm(p + 1, t, flags, 0))
                                        return 0;
                                if (!*t || is_sep(*t, flags))
                                        return FNM_NOMATCH;
                        }
                case '[': {
                        const char *q = p + 1;
                        int neg = *q == '!' || *q == '^', ok = 0;
                        if (neg)
                                q++;
                        if (!*s || is_sep(*s, flags))
                                return FNM_NOMATCH;
                        do {
                                char lo = *q, hi = lo;
                                if (!lo)
                                        return FNM_NOMATCH; /* unterminated */
                                if (q[1] == '-' && q[2] && q[2] != ']') {
                                        hi = q[2];
                                        q += 2;
                                }
                                if (*s >= lo && *s <= hi)
                                        ok = 1;
                                q++;
                        } while (*q != ']');
                        if (ok == neg)
                                return FNM_NOMATCH;
                        p = q;
                        break;
                }
                case '\\':
                        if (!(flags & FNM_NOESCAPE) && p[1])
                                c = *++p;
                        /* fall through */
                default:
                        if (c != *s)
                                return FNM_NOMATCH;
                }
        }
}

int fnmatch(const char *pattern, const char *string, int flags)
{
        return fnm(pattern, string, flags, 1);
}

/* ---- regex.h on GRegex ---------------------------------------------------- */

int regcomp(regex_t *preg, const char *regex, int cflags)
{
        GRegexCompileFlags f = G_REGEX_DEFAULT;
        if (cflags & REG_ICASE)
                f |= G_REGEX_CASELESS;
        if (cflags & REG_NEWLINE)
                f |= G_REGEX_MULTILINE;
        GError *err = NULL;
        GRegex *re = g_regex_new(regex, f, G_REGEX_MATCH_DEFAULT, &err);
        if (!re) {
                g_error_free(err);
                preg->re = NULL;
                return REG_BADPAT;
        }
        preg->re = re;
        preg->re_nsub = (size_t)g_regex_get_capture_count(re);
        preg->nosub = (cflags & REG_NOSUB) != 0;
        return 0;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags)
{
        (void)eflags;
        GMatchInfo *mi = NULL;
        gboolean ok = g_regex_match(preg->re, string, G_REGEX_MATCH_DEFAULT, &mi);
        if (ok && !preg->nosub) {
                for (size_t i = 0; i < nmatch; i++) {
                        gint s = -1, e = -1;
                        if (!g_match_info_fetch_pos(mi, (gint)i, &s, &e))
                                s = e = -1;
                        pmatch[i].rm_so = s;
                        pmatch[i].rm_eo = e;
                }
        }
        g_match_info_free(mi);
        return ok ? 0 : REG_NOMATCH;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
        (void)preg;
        const char *msg = errcode == REG_NOMATCH ? "no match" : "invalid regular expression";
        if (errbuf_size)
                g_strlcpy(errbuf, msg, errbuf_size);
        return strlen(msg) + 1;
}

void regfree(regex_t *preg)
{
        if (preg->re)
                g_regex_unref(preg->re);
        preg->re = NULL;
}

/* ---- wordexp ------------------------------------------------------------- */

int wordexp(const char *words, wordexp_t *we, int flags)
{
        GString *out = g_string_new(NULL);
        const char *p = words;

        if (p[0] == '~' && (p[1] == 0 || p[1] == '/' || p[1] == '\\')) {
                g_string_append(out, g_get_home_dir());
                p++;
        }
        while (*p) {
                if (*p == '`' || (p[0] == '$' && p[1] == '(')) {
                        g_string_free(out, TRUE);
                        return WRDE_CMDSUB;
                }
                const char *name_start = NULL, *name_end = NULL;
                const char *next = NULL;
                if (p[0] == '$' && p[1] == '{') {
                        name_start = p + 2;
                        name_end = strchr(name_start, '}');
                        if (!name_end) {
                                g_string_free(out, TRUE);
                                return WRDE_SYNTAX;
                        }
                        next = name_end + 1;
                } else if (p[0] == '$' && (g_ascii_isalpha(p[1]) || p[1] == '_')) {
                        name_start = p + 1;
                        name_end = name_start;
                        while (g_ascii_isalnum(*name_end) || *name_end == '_')
                                name_end++;
                        next = name_end;
                } else if (p[0] == '%') {
                        const char *close = strchr(p + 1, '%');
                        if (close && close > p + 1) {
                                name_start = p + 1;
                                name_end = close;
                                next = close + 1;
                        }
                }
                if (name_start) {
                        char *name = g_strndup(name_start, name_end - name_start);
                        const char *val = g_getenv(name);
                        g_free(name);
                        if (!val && (flags & WRDE_UNDEF) && p[0] == '$') {
                                g_string_free(out, TRUE);
                                return WRDE_BADVAL;
                        }
                        g_string_append(out, val ? val : "");
                        p = next;
                } else {
                        g_string_append_c(out, *p++);
                }
        }
        we->we_wordc = 1;
        we->we_wordv = g_new0(char *, 2);
        we->we_wordv[0] = g_string_free(out, FALSE);
        we->we_offs = 0;
        return 0;
}

void wordfree(wordexp_t *we)
{
        g_strfreev(we->we_wordv);
        we->we_wordv = NULL;
        we->we_wordc = 0;
}

/* ---- scandir --------------------------------------------------------------- */

int alphasort(const struct dirent **a, const struct dirent **b)
{
        return g_utf8_collate((*a)->d_name, (*b)->d_name);
}

int scandir(const char *dir, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **))
{
        GDir *d = g_dir_open(dir, 0, NULL);
        if (!d)
                return -1;
        GPtrArray *arr = g_ptr_array_new();
        const char *name;
        while ((name = g_dir_read_name(d))) {
                struct dirent *e = calloc(1, sizeof *e);
                g_strlcpy(e->d_name, name, sizeof e->d_name);
                if (filter && !filter(e)) {
                        free(e);
                        continue;
                }
                g_ptr_array_add(arr, e);
        }
        g_dir_close(d);
        if (compar)
                qsort(arr->pdata, arr->len, sizeof(void *),
                      (int (*)(const void *, const void *))compar);
        int n = (int)arr->len;
        /* caller frees with free(): hand over a malloc'd copy of the array */
        *namelist = malloc(sizeof(struct dirent *) * (n ? n : 1));
        memcpy(*namelist, arr->pdata, sizeof(struct dirent *) * n);
        g_ptr_array_free(arr, TRUE);
        return n;
}
