#ifndef CMDPLAY_CONFIG_H
#define CMDPLAY_CONFIG_H

#include <stdbool.h>

#define MAX_CHANNELS 64

enum sample_format_mode { FMT_NATIVE, FMT_S32 };
enum pause_mode { PAUSE_REOPEN, PAUSE_DEVICE };

typedef struct {
    // [output]
    char *device;
    char *alsa_conf;
    int sample_format;
    unsigned buffer_ms;
    unsigned period_ms;
    unsigned channels;
    int channel_map[MAX_CHANNELS];
    unsigned n_channel_map;
    bool allow_resample;
    // Audio held after the device (e.g. inside CamillaDSP): both are ad
    unsigned latency_ms;
    unsigned latency_frames;
    // [player]
    bool gapless;
    int pause_mode;
    unsigned seek_step;
    unsigned seek_step_long;
    bool keys;
    char *text_encodings;
    // Read each file into memory before playing it; larger files stream.
    bool preload;
    unsigned preload_max_mb;
} settings_t;

void settings_defaults(settings_t *s);
// Returns 0, or -1 after reporting the problem.
int settings_load(settings_t *s, const char *path);
// $XDG_CONFIG_HOME/cmdplay/cmdplay.conf or ~/.config/cmdplay/cmdplay.con
// NULL if it does not exist.
char *settings_default_path(void);
void settings_free(settings_t *s);

#endif