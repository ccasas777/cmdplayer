#ifndef CMDPLAY_PRELOAD_H
#define CMDPLAY_PRELOAD_H

#include <stdbool.h>
#include <stddef.h>

// A whole file in memory.
typedef struct {
    unsigned char *data;
    size_t size;
} blob_t;

void blob_free(blob_t *b);

// A file being read into memory by a background thread.
typedef struct preload preload_t;

// Starts reading path; files larger than max_bytes are not read.
preload_t *preload_start(const char *path, size_t max_bytes);
const char *preload_path(const preload_t *l);
bool preload_done(const preload_t *l);
// Waits for the read and frees l.  Returns the contents, or NULL with *why
// set to a static reason when the file is too large or cannot be read.
blob_t *preload_finish(preload_t *l, const char **why);
// Abandons the read and frees l; NULL is ignored.
void preload_cancel(preload_t *l);

#endif