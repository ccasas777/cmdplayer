#define _GNU_SOURCE
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <iconv.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static void oom(void) {
    fputs("cmdplay: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) oom();
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) oom();
    return p;
}

char *xstrdup(const char *s) {
    char *d = strdup(s);
    if (!d) oom();
    return d;
}

char *xasprintf(const char *fmt, ...) {
    va_list ap;
    char *s;
    va_start(ap, fmt);
    int r = vasprintf(&s, fmt, ap);
    va_end(ap);
    if (r < 0) oom();
    return s;
}

char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (cap - n < 4096) buf = xrealloc(buf, cap *= 2);
        size_t r = fread(buf + n, 1, cap - n - 1, f);
        n += r;
        if (r == 0) break;
    }
    int err = ferror(f) ? EIO : 0;
    fclose(f);
    if (err) {
        free(buf);
        errno = err;
        return NULL;
    }
    buf[n] = '\0';
    if (len) *len = n;
    return buf;
}

char *path_resolve(const char *path, const char *base_dir) {
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
        return xasprintf("%s%s", home ? home : "", path + 1);
    }
    if (path[0] == '/' || !base_dir || !base_dir[0]) return xstrdup(path);
    return xasprintf("%s/%s", base_dir, path);
}

char *path_dirname(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return xstrdup(".");
    if (slash == path) return xstrdup("/");
    return strndup(path, (size_t)(slash - path));
}

const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool str_iends(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcasecmp(s + n - m, suffix) == 0;
}

char *str_trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

bool utf8_valid(const char *str, size_t n) {
    const unsigned char *s = (const unsigned char *)str;
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        size_t len;
        unsigned min;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) { len = 2; min = 0x80; c &= 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; min = 0x800; c &= 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; min = 0x10000; c &= 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            c = (c << 6) | (s[i + k] & 0x3F);
        }
        if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

static char *iconv_whole(const char *buf, size_t n, const char *from) {
    iconv_t cd = iconv_open("UTF-8", from);
    if (cd == (iconv_t)-1) return NULL;
    size_t cap = n * 4 + 1;
    char *out = xmalloc(cap);
    char *in = (char *)buf, *o = out;
    size_t inleft = n, outleft = cap - 1;
    size_t r = iconv(cd, &in, &inleft, &o, &outleft);
    iconv_close(cd);
    if (r == (size_t)-1 || inleft != 0) {
        free(out);
        return NULL;
    }
    *o = '\0';
    return out;
}

char *text_to_utf8(const char *buf, size_t n, const char *encodings,
                   const char **used) {
    static char name[64];
    if (n >= 3 && memcmp(buf, "\xEF\xBB\xBF", 3) == 0) {
        buf += 3;
        n -= 3;
    }
    if (utf8_valid(buf, n)) {
        if (used) *used = "UTF-8";
        return strndup(buf, n);
    }
    const char *p = encodings ? encodings : "";
    while (*p) {
        size_t len = strcspn(p, ",");
        if (len > 0 && len < sizeof(name)) {
            memcpy(name, p, len);
            name[len] = '\0';
            char *enc = str_trim(name);
            char *out = enc ? iconv_whole(buf, n, enc) : NULL;
            if (out) {
                if (used) *used = enc;
                return out;
            }
        }
        p += len;
        if (*p == ',') p++;
    }
    return NULL;
}