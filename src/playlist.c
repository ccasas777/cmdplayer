#define _GNU_SOURCE
#include "playlist.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ui.h"
#include "util.h"

static track_t *push(playlist_t *pl) {
    if (pl->n == pl->cap) {
        pl->cap = pl->cap ? pl->cap * 2 : 64;
        pl->items = xrealloc(pl->items, pl->cap * sizeof(track_t));
    }
    track_t *t = &pl->items[pl->n++];
    memset(t, 0, sizeof(*t));
    t->end_cd = -1;
    return t;
}

static void track_free(track_t *t) {
    free(t->path);
    free(t->title);
    free(t->artist);
    free(t->album);
}

void playlist_free(playlist_t *pl) {
    for (size_t i = 0; i < pl->n; i++) track_free(&pl->items[i]);
    free(pl->items);
    memset(pl, 0, sizeof(*pl));
}

static const char *audio_exts[] = {
    ".flac", ".ape", ".wv", ".wav", ".m4a", ".tta", ".tak", ".aiff", ".aif",
    ".dsf", ".dff", ".mp3", ".ogg", ".opus", NULL,
};

static char *strip_ext(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    if (!dot || (slash && dot < slash)) return xstrdup(path);
    return strndup(path, (size_t)(dot - path));
}

static char *find_with_exts(const char *stem) {
    for (size_t i = 0; audio_exts[i]; i++) {
        char *p = xasprintf("%s%s", stem, audio_exts[i]);
        if (path_exists(p)) return p;
        free(p);
    }
    return NULL;
}

// CUE FILE entries often name the original rip (.wav) while the file next
// to the sheet was converted later, or named after the sheet itself.
static char *cue_find_file(const char *name, const char *cue_path, const char *dir) {
    char *p = path_resolve(name, dir);
    if (path_exists(p)) return p;
    char *stem = strip_ext(p);
    char *found = find_with_exts(stem);
    free(stem);
    if (!found) {
        stem = strip_ext(cue_path);
        found = find_with_exts(stem);
        free(stem);
    }
    if (found) {
        ui_dbg("CUE file %s not found, using %s", p, found);
        free(p);
        return found;
    }
    return p;
}

// Argument after a CUE keyword: a quoted string, or the next word.
static char *cue_arg(const char *s, const char **rest) {
    while (*s == ' ' || *s == '\t') s++;
    const char *end;
    char *v;
    if (*s == '"') {
        end = strchr(s + 1, '"');
        if (!end) end = s + strlen(s);
        v = strndup(s + 1, (size_t)(end - s - 1));
        if (*end) end++;
    } else {
        end = s + strcspn(s, " \t");
        v = strndup(s, (size_t)(end - s));
    }
    if (rest) *rest = end;
    return v;
}

static int64_t cue_time(const char *s) {
    int m, sec, f;
    if (sscanf(s, "%u:%u:%u", &m, &sec, &f) != 3) return -1;
    return ((int64_t)m * 60 + sec) * CD_FRAMES_PER_SEC + f;
}

static bool cue_track_selected(int track, const int *selected, size_t selected_n) {
    if (selected_n == 0) return true;
    for (size_t i = 0; i < selected_n; i++) {
        if (selected[i] == track) return true;
    }
    return false;
}

static void set_str(char **dst, char *v) {
    free(*dst);
    *dst = v;
}

static int cue_load(playlist_t *pl, const char *cue_path, const char *encodings,
                    const int *selected, size_t selected_n) {
    size_t len;
    char *raw = read_file(cue_path, &len);
    if (!raw) {
        ui_err("cannot read %s: %s", cue_path, strerror(errno));
        return -1;
    }
    const char *enc = NULL;
    char *text = text_to_utf8(raw, len, encodings, &enc);
    free(raw);
    if (!text) {
        ui_err("%s: text encoding is not UTF-8 or one of %s", cue_path, encodings);
        return -1;
    }
    if (strcmp(enc, "UTF-8") != 0) ui_dbg("%s: read as %s", cue_path, enc);

    char *dir = path_dirname(cue_path);
    char *album = NULL, *album_artist = NULL, *file = NULL;
    size_t first = pl->n;
    track_t *cur = NULL;
    bool cur_audio = false;

    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        char *l = str_trim(line);
        size_t kw_len = strcspn(l, " \t");
        if (kw_len == 0) continue;
        const char *args = "";
        if (l[kw_len]) {
            l[kw_len] = '\0';
            args = l + kw_len + 1;
        }

        if (strcasecmp(l, "FILE") == 0) {
            char *name = cue_arg(args, NULL);
            set_str(&file, cue_find_file(name, cue_path, dir));
            free(name);
        } else if (strcasecmp(l, "TRACK") == 0) {
            const char *rest;
            char *num = cue_arg(args, &rest);
            char *type = cue_arg(rest, NULL);
            cur_audio = strcasecmp(type, "AUDIO") == 0;
            cur = NULL;
            if (cur_audio && file) {
                cur = push(pl);
                cur->path = xstrdup(file);
                cur->cue_track = atoi(num);
                cur->start_cd = -1;
                if (album) cur->album = xstrdup(album);
                if (album_artist) cur->artist = xstrdup(album_artist);
            }
            free(num);
            free(type);
        } else if (strcasecmp(l, "TITLE") == 0) {
            char *v = cue_arg(args, NULL);
            if (cur) set_str(&cur->title, v);
            else if (!cur_audio && pl->n == first) set_str(&album, v);
            else free(v);
        } else if (strcasecmp(l, "PERFORMER") == 0) {
            char *v = cue_arg(args, NULL);
            if (cur) set_str(&cur->artist, v);
            else if (!cur_audio && pl->n == first) set_str(&album_artist, v);
            else free(v);
        } else if (strcasecmp(l, "INDEX") == 0 && cur) {
            const char *rest;
            char *num = cue_arg(args, &rest);
            char *t = cue_arg(rest, NULL);
            if (atoi(num) == 1) cur->start_cd = cue_time(t);
            else if (atoi(num) == 0 && cur->start_cd < 0) cur->start_cd = -2 - cue_time(t);
            free(num);
            free(t);
        }
    }

    // Tracks without INDEX 01 start at their INDEX 00, encoded as -2 - time.
    for (size_t i = first; i < pl->n; i++) {
        track_t *t = &pl->items[i];
        if (t->start_cd <= -2) t->start_cd = -2 - t->start_cd;
    }

    // Keep boundaries from unselected tracks, otherwise a selected track in a
    // single-file image would continue through the rest of the image.
    for (size_t i = first; i < pl->n; i++) {
        track_t *t = &pl->items[i];
        if (i + 1 < pl->n && strcmp(pl->items[i + 1].path, t->path) == 0)
            t->end_cd = pl->items[i + 1].start_cd;
    }

    size_t out = first;
    for (size_t i = first; i < pl->n; i++) {
        track_t *t = &pl->items[i];
        if (!cue_track_selected(t->cue_track, selected, selected_n)) {
            track_free(t);
            continue;
        }
        if (t->start_cd < 0) {
            ui_err("%s: track %d has no INDEX, skipped", cue_path, t->cue_track);
            track_free(t);
            continue;
        }
        pl->items[out++] = *t;
    }
    pl->n = out;
    // The pregap (INDEX 00) stays with the previous track.
    for (size_t i = first; i < pl->n; i++) {
        track_t *t = &pl->items[i];
        if (!path_exists(t->path)) {
            ui_err("%s: audio file %s not found", cue_path, t->path);
        }
    }

    size_t added = pl->n - first;
    free(album);
    free(album_artist);
    free(file);
    free(dir);
    free(text);
    if (added == 0) {
        ui_err("%s: no audio tracks", cue_path);
        return -1;
    }
    return 0;
}

int playlist_add(playlist_t *pl, const char *path, const char *encodings) {
    int *selected = NULL;
    size_t selected_n = 0;
    const char *cue_suffix = NULL;
    for (const char *p = path; *p; p++) {
        if (strncasecmp(p, ".cue:", 5) == 0) {
            cue_suffix = p;
            break;
        }
    }
    if (cue_suffix) {
        char *end;
        const char *number = cue_suffix + 5;
        while (*number) {
            long track = strtol(number, &end, 10);
            if (end == number || track < 1 || track > INT_MAX ||
                (*end != ':' && *end != '\0')) {
                ui_err("invalid CUE track selection: %s", path);
                free(selected);
                return -1;
            }
            selected = xrealloc(selected, (selected_n + 1) * sizeof(*selected));
            selected[selected_n++] = (int)track;
            number = *end == ':' ? end + 1 : end;
        }
        char *cue_path = strndup(path, (size_t)(cue_suffix - path) + 4);
        int result = cue_load(pl, cue_path, encodings, selected, selected_n);
        free(cue_path);
        free(selected);
        return result;
    }
    if (str_iends(path, ".cue")) return cue_load(pl, path, encodings, NULL, 0);
    if (!path_exists(path)) {
        ui_err("not found: %s", path);
        return -1;
    }
    track_t *t = push(pl);
    t->path = xstrdup(path);
    return 0;
}

int playlist_load(playlist_t *pl, const char *list_path, const char *encodings) {
    size_t len;
    char *raw = read_file(list_path, &len);
    if (!raw) {
        ui_err("cannot read playlist %s: %s", list_path, strerror(errno));
        return -1;
    }
    const char *enc = NULL;
    char *text = text_to_utf8(raw, len, encodings, &enc);
    free(raw);
    if (!text) {
        ui_err("%s: text encoding is not UTF-8 or one of %s", list_path, encodings);
        return -1;
    }
    if (strcmp(enc, "UTF-8") != 0) ui_dbg("%s: read as %s", list_path, enc);
    char *dir = path_dirname(list_path);
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        char *l = str_trim(line);
        if (!*l || *l == '#') continue;
        if (strncmp(l, "file://", 7) == 0) l += 7;
        char *p = path_resolve(l, dir);
        playlist_add(pl, p, encodings);
        free(p);
    }
    free(dir);
    free(text);
    return 0;
}