#ifndef CMDPLAY_OUTPUT_H
#define CMDPLAY_OUTPUT_H

#include <stdbool.h>

#include <alsa/asoundlib.h>

#include "config.h"

typedef struct output output_t;

// Loads the settings' alsa_conf, if any. Returns NULL after reporting.
output_t *out_new(const settings_t *s);
void out_free(output_t *o);

// Opens the device for a stream. fmt is tried first; if the device does
// not take it, S32_LE. Returns 0 or a negative error after reporting.
int out_open(output_t *o, snd_pcm_format_t fmt, unsigned rate, unsigned channels);
bool out_is_open(const output_t *o);
// True when a stream with these parameters can continue on the open device.
bool out_matches(const output_t *o, snd_pcm_format_t fmt, unsigned rate, unsigned channels);
// Format the samples given to out_write must have.
snd_pcm_format_t out_format(const output_t *o);
const char *out_describe(const output_t *o);

// Writes interleaved frames with the stream's channel count. Returns 0 or
// a negative error the device did not recover from.
int out_write(output_t *o, const void *buf, long frames);
// Frames written and not yet played, as far as the device knows.
long out_delay(const output_t *o);
// Frames to let play before out_drain_close, so that it blocks only for
// the last periods. Waiting longer would risk an underrun, which stops the
// stream before its end has played.
long out_draining_left(const output_t *o);
// Plays what is queued, then closes.
void out_drain_close(output_t *o);
// Discards what is queued and closes.
void out_drop_close(output_t *o);
// Pauses in the device. -ENOSYS when the device cannot pause.
int out_pause(output_t *o, bool on);

#endif