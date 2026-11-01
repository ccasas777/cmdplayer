#define _GNU_SOURCE
#include "preload.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

#define READ_STEP (4u << 20)

struct preload {
    char *path;
    size_t max_bytes;
    pthread_t thread;
    bool threaded;
    atomic_bool cancel;
    atomic_bool done;
    blob_t *blob;
    const char *why;
};

void blob_free(blob_t *b) {
    if (!b) return;
    free(b->data);
    free(b);
}

// Runs without touching the UI, which is not thread safe.
static void *load(void *arg) {
    preload_t *l = arg;
    int fd = open(l->path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        l->why = "cannot read the file";
        goto out;
    }
    if ((unsigned long long)st.st_size > l->max_bytes) {
        l->why = "larger than preload_max";
        goto out;
    }
    size_t size = (size_t)st.st_size;
    unsigned char *data = malloc(size ? size : 1);
    if (!data) {
        l->why = "not enough memory";
        goto out;
    }
    size_t got = 0;
    while (got < size) {
        if (atomic_load(&l->cancel)) {
            free(data);
            goto out;
        }
        size_t want = size - got < READ_STEP ? size - got : READ_STEP;
        ssize_t n = read(fd, data + got, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            free(data);
            l->why = "cannot read the file";
            goto out;
        }
        got += (size_t)n;
    }
    // The copy in memory is what plays; the page cache need not keep another.
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    l->blob = xmalloc(sizeof(*l->blob));
    l->blob->data = data;
    l->blob->size = size;
out:
    if (fd >= 0) close(fd);
    atomic_store(&l->done, true);
    return NULL;
}

preload_t *preload_start(const char *path, size_t max_bytes) {
    preload_t *l = xcalloc(1, sizeof(*l));
    l->path = xstrdup(path);
    l->max_bytes = max_bytes;
    atomic_init(&l->cancel, false);
    atomic_init(&l->done, false);
    l->threaded = pthread_create(&l->thread, NULL, load, l) == 0;
    if (!l->threaded) load(l);
    return l;
}

const char *preload_path(const preload_t *l) { return l->path; }

bool preload_done(const preload_t *l) { return atomic_load(&l->done); }

static void join(preload_t *l) {
    if (l->threaded) pthread_join(l->thread, NULL);
}

blob_t *preload_finish(preload_t *l, const char **why) {
    join(l);
    blob_t *b = l->blob;
    *why = l->why;
    free(l->path);
    free(l);
    return b;
}

void preload_cancel(preload_t *l) {
    if (!l) return;
    atomic_store(&l->cancel, true);
    join(l);
    blob_free(l->blob);
    free(l->path);
    free(l);
}