#ifndef CMDPLAY_UTIL_H
#define CMDPLAY_UTIL_H

#include <stdbool.h>
#include <stddef.h>

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Whole file into a NUL terminated buffer. Returns NULL and sets errno.
char *read_file(const char *path, size_t *len);

// "~/x" to $HOME/x; relative paths are joined to base_dir when it is set.
char *path_resolve(const char *path, const char *base_dir);
char *path_dirname(const char *path);
const char *path_basename(const char *path);
bool path_exists(const char *path);

bool str_iends(const char *s, const char *suffix);
char *str_trim(char *s);

bool utf8_valid(const char *s, size_t n);
// Converts text to UTF-8. UTF-8 input (with or without BOM) is copied,
// otherwise each encoding of the comma separated list is tried in order.
// Returns NULL when none of them converts the whole text.
char *text_to_utf8(const char *buf, size_t n, const char *encodings,
                   const char **used);

#endif