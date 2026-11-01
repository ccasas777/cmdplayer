#ifndef CMDPLAY_DECODER_H
#define CMDPLAY_DECODER_H

#include <stdbool.h>
#include <stdint.h>

#include <alsa/asoundlib.h>

#include "preload.h"

typedef struct {
    unsigned rate;
    unsigned channels;
    // Significant bits of integer sources, 0 for float or unknown.
    int bits;
    bool is_float;
    // Lossless output format: S16_LE, S32_LE, FLOAT_LE or FLOAT64_LE.
    snd_pcm_format_t native;
    // Total frames, -1 if unknown.
    int64_t frames;
    char codec[32];
    char *title, *artist, *album;
} dec_info_t;

typedef struct decoder decoder_t;

// Sends FFmpeg's log through the UI.
void dec_global_init(void);
// Decodes path from mem, its whole contents, or from the disk when mem is
// NULL. Takes ownership of mem. Reports the problem and returns NULL on
// failure.
decoder_t *dec_open(const char *path, blob_t *mem);
// Whether the file is decoded from memory.
bool dec_in_memory(const decoder_t *d);
const dec_info_t *dec_info(const decoder_t *d);
const char *dec_path(const decoder_t *d);
// Positions the decoder so the next sample read is file frame `frame`.
int dec_seek(decoder_t *d, int64_t frame);
// File frame of the next sample dec_read returns.
int64_t dec_pos(const decoder_t *d);
// Reads up to max frames as interleaved samples in fmt. Returns the number
// of frames, 0 at the end of the file, or a negative error.
long dec_read(decoder_t *d, void *buf, long max, snd_pcm_format_t fmt);
void dec_close(decoder_t *d);

#endif