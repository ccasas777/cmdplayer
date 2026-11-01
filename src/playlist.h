#ifndef CMDPLAY_PLAYLIST_H
#define CMDPLAY_PLAYLIST_H

#include <stddef.h>
#include <stdint.h>

#define CD_FRAMES_PER_SEC 75

typedef struct {
    char *path;
    // Set for CUE tracks; other files take them from their tags.
    char *title;
    char *artist;
    char *album;
    int cue_track;
    // Position in the file in CD frames (1/75 s); end_cd < 0 is end of file.
    int64_t start_cd;
    int64_t end_cd;
} track_t;

typedef struct {
    track_t *items;
    size_t n, cap;
} playlist_t;

// A list file has one path per line, relative to the list. Empty lines and
// lines starting with # are skipped, so m3u/m3u8 files work too. Paths
// ending in .cue are expanded to all tracks; .cue:N:M selects tracks N and M.
int playlist_load(playlist_t *pl, const char *list_path, const char *encodings);
// A single audio file or CUE sheet.
int playlist_add(playlist_t *pl, const char *path, const char *encodings);
void playlist_free(playlist_t *pl);

#endif