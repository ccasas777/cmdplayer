#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "decoder.h"
#include "output.h"
#include "playlist.h"
#include "ui.h"
#include "util.h"

#define CHUNK_FRAMES 4096
#define MAX_MARKS 16
// "Previous" restarts the current track after this many seconds.
#define PREV_RESTART_SEC 3

// Where a track's audio begins in the output stream.
typedef struct {
    size_t track;
    int64_t at;      // output frame
    int64_t offset;  // track frame written at `at`
    int64_t length;  // track frames, -1 if unknown
    unsigned rate;
    char *announce;
} mark_t;

typedef struct {
    settings_t *s;
    playlist_t *pl;
    output_t *out;
    decoder_t *dec;
    // The next file to play, being read into memory.
    preload_t *next;
    // Track cur is decoded and waits for the previous one to play out.
    bool waiting;
    size_t cur;
    int64_t cur_start, cur_end;
    int64_t written;
    mark_t marks[MAX_MARKS];
    int n_marks;
    size_t announced;
    bool paused, device_paused;
    size_t pause_track;
    int64_t pause_pos;
    // Of the paused track, kept while the output is closed.
    unsigned pause_rate;
    int64_t pause_length;
    bool goto_mode;
    char goto_buf[12];
    void *buf;
    size_t buf_size;
} player_t;

static volatile sig_atomic_t quit_signal;

static void on_signal(int sig) {
    (void)sig;
    quit_signal = 1;
}

static void clear_marks(player_t *p) {
    for (int i = 0; i < p->n_marks; i++) free(p->marks[i].announce);
    p->n_marks = 0;
    p->written = 0;
}

static void add_mark(player_t *p, mark_t m) {
    if (p->n_marks == MAX_MARKS) {
        free(p->marks[0].announce);
        memmove(p->marks, p->marks + 1, sizeof(mark_t) * (MAX_MARKS - 1));
        p->n_marks--;
    }
    p->marks[p->n_marks++] = m;
}

static int64_t cd_to_frames(int64_t cd, unsigned rate) {
    return cd * rate / CD_FRAMES_PER_SEC;
}

static void fmt_time(char *buf, size_t n, int64_t frames, unsigned rate) {
    if (frames < 0 || !rate) {
        snprintf(buf, n, "--:--");
        return;
    }
    int64_t sec = frames / rate;
    if (sec >= 3600)
        snprintf(buf, n, "%d:%02d:%02d", (int)(sec / 3600), (int)(sec / 60 % 60), (int)(sec % 60));
    else
        snprintf(buf, n, "%02d:%02d", (int)(sec / 60), (int)(sec % 60));
}

static char *track_label(const player_t *p, size_t idx, const dec_info_t *in) {
    const track_t *t = &p->pl->items[idx];
    const char *title = t->title ? t->title : in && in->title ? in->title : NULL;
    const char *artist = t->artist ? t->artist : in && in->artist ? in->artist : NULL;
    char *name;
    if (title && artist)
        name = xasprintf("%s - %s", artist, title);
    else if (title)
        name = xstrdup(title);
    else
        name = xstrdup(path_basename(t->path));
    char *s = xasprintf("[%zu/%zu] %s", idx + 1, p->pl->n, name);
    free(name);
    return s;
}

static snd_pcm_format_t wanted_format(const settings_t *s, const dec_info_t *in) {
    return s->sample_format == FMT_S32 ? SND_PCM_FORMAT_S32_LE : in->native;
}

// Track and track frame that is being heard now.
static bool heard(const player_t *p, size_t *track, int64_t *pos) {
    if (p->n_marks == 0) return false;
    const mark_t *m = &p->marks[0];
    int64_t latency = (int64_t)p->s->latency_ms * m->rate / 1000 + p->s->latency_frames;
    int64_t played = p->written - out_delay(p->out) - latency;
    if (played < 0) played = 0;
    for (int i = 1; i < p->n_marks; i++)
        if (p->marks[i].at <= played) m = &p->marks[i];
    *track = m->track;
    *pos = m->offset + (played - m->at);
    if (m->length >= 0 && *pos > m->length) *pos = m->length;
    return true;
}

static const mark_t *heard_mark(const player_t *p, size_t track) {
    for (int i = p->n_marks - 1; i >= 0; i--)
        if (p->marks[i].track == track) return &p->marks[i];
    return NULL;
}

// Sample rate and length of a track that is playing or paused; 0 and -1
// when they are not known.
static void timing(const player_t *p, size_t track, unsigned *rate, int64_t *length) {
    const mark_t *m = heard_mark(p, track);
    if (m) {
        *rate = m->rate;
        *length = m->length;
    } else if (p->paused && track == p->pause_track) {
        *rate = p->pause_rate;
        *length = p->pause_length;
    } else {
        *rate = 0;
        *length = -1;
    }
}

static void close_decoder(player_t *p) {
    dec_close(p->dec);
    p->dec = NULL;
}

static size_t preload_bytes(const player_t *p) {
    return (size_t)p->s->preload_max_mb << 20;
}

// Opens path, from memory when preloading.
static decoder_t *open_decoder(player_t *p, const char *path) {
    if (!p->s->preload) return dec_open(path, NULL);
    preload_t *l = p->next;
    p->next = NULL;
    if (l && strcmp(preload_path(l), path) != 0) {
        preload_cancel(l);
        l = NULL;
    }
    if (!l) l = preload_start(path, preload_bytes(p));
    if (!preload_done(l)) ui_status("loading %s", path_basename(path));
    const char *why = NULL;
    blob_t *mem = preload_finish(l, &why);
    if (!mem) ui_dbg("%s: playing from disk: %s", path, why);
    return dec_open(path, mem);
}

// Starts reading the first file after track idx that is not idx's own.
static void preload_after(player_t *p, size_t idx) {
    if (!p->s->preload) return;
    const char *path = p->pl->items[idx].path;
    size_t j = idx + 1;
    while (j < p->pl->n && strcmp(p->pl->items[j].path, path) == 0) j++;
    const char *want = j < p->pl->n ? p->pl->items[j].path : NULL;
    if (p->next && want && strcmp(preload_path(p->next), want) == 0) return;
    preload_cancel(p->next);
    p->next = want ? preload_start(want, preload_bytes(p)) : NULL;
}

static void stop_output(player_t *p) {
    out_drop_close(p->out);
    clear_marks(p);
    p->waiting = false;
}

// Stops the output and stays paused at track frame pos until a key press
// retries from there.
static void hold(player_t *p, size_t track, int64_t pos, unsigned rate, int64_t length,
                 const char *why) {
    stop_output(p);
    p->paused = true;
    p->device_paused = false;
    p->pause_track = track;
    p->pause_pos = pos;
    p->pause_rate = rate;
    p->pause_length = length;
    ui_msg("%s; press space to retry", why);
}

enum { STARTED, SKIPPED, HELD, WAITING };

// Starts decoding track idx at track frame pos. A continuous start follows
// the previous track without a gap when the output format allows it, or
// else after the previous track has played out; any other start discards
// what is queued in the output. SKIPPED means the track cannot be played,
// HELD that the device failed and the player waits paused at the track,
// WAITING that the previous track is still playing out (p->waiting) and
// the start is to be repeated when it has.
static int start_track(player_t *p, size_t idx, int64_t pos, bool continuous) {
    const track_t *t = &p->pl->items[idx];
    if (p->dec && strcmp(dec_path(p->dec), t->path) != 0) close_decoder(p);
    if (!p->dec && !(p->dec = open_decoder(p, t->path))) return SKIPPED;
    const dec_info_t *in = dec_info(p->dec);

    p->cur = idx;
    p->cur_start = cd_to_frames(t->start_cd, in->rate);
    p->cur_end = t->end_cd >= 0 ? cd_to_frames(t->end_cd, in->rate) : -1;
    int64_t length = p->cur_end >= 0 ? p->cur_end - p->cur_start
                   : in->frames >= 0 ? in->frames - p->cur_start : -1;

    if (pos < 0) pos = 0;
    if (dec_seek(p->dec, p->cur_start + pos) < 0) {
        close_decoder(p);
        return SKIPPED;
    }

    snd_pcm_format_t fmt = wanted_format(p->s, in);
    bool keep = continuous && p->s->gapless
        && out_matches(p->out, fmt, in->rate, in->channels);
    if (!keep) {
        if (out_is_open(p->out)) {
            if (continuous && out_draining_left(p->out) > 0) {
                // Keys keep working while the previous track plays out.
                p->waiting = true;
                return WAITING;
            }
            if (continuous) {
                ui_status("waiting for the device to finish the previous track");
                out_drain_close(p->out);
            } else {
                out_drop_close(p->out);
            }
        }
        clear_marks(p);
        int r = out_open(p->out, fmt, in->rate, in->channels);
        if (r < 0) {
            unsigned rate = in->rate;
            close_decoder(p);
            if (r == -EINVAL) return SKIPPED;
            hold(p, idx, pos, rate, length, "cannot open the output");
            return HELD;
        }
    }

    size_t need = (size_t)CHUNK_FRAMES * in->channels * 8;
    if (need > p->buf_size) {
        p->buf = xrealloc(p->buf, need);
        p->buf_size = need;
    }

    char *label = track_label(p, idx, in);
    char bits[16] = "float";
    if (!in->is_float) snprintf(bits, sizeof(bits), "%d bit", in->bits);
    char len[16];
    fmt_time(len, sizeof(len), length, in->rate);
    mark_t m = {
        .track = idx,
        .at = p->written,
        .offset = pos,
        .length = length,
        .rate = in->rate,
        .announce = xasprintf("%s (%s)\n          %s %s %u Hz %u ch -> %s", label, len,
                              in->codec, bits, in->rate, in->channels,
                              snd_pcm_format_name(out_format(p->out))),
    };
    free(label);
    add_mark(p, m);
    preload_after(p, idx);
    return STARTED;
}

// Starts the first playable track from idx on, or holds at it. False at the
// end of the list.
static bool advance(player_t *p, size_t idx, bool continuous) {
    for (; idx < p->pl->n; idx++) {
        if (start_track(p, idx, 0, continuous) != SKIPPED) return true;
        ui_msg("skipping track %zu", idx + 1);
    }
    return false;
}

static void jump(player_t *p, size_t idx, int64_t pos) {
    p->paused = p->device_paused = false;
    stop_output(p);
    if (pos > 0) {
        if (start_track(p, idx, pos, false) != SKIPPED) return;
        ui_msg("skipping track %zu", idx + 1);
        idx++;
    }
    if (!advance(p, idx, false)) {
        close_decoder(p);
        ui_msg("nothing left to play");
    }
}

static void toggle_pause(player_t *p) {
    if (p->paused) {
        p->paused = false;
        if (p->device_paused && out_pause(p->out, false) == 0) {
            p->device_paused = false;
            return;
        }
        p->device_paused = false;
        jump(p, p->pause_track, p->pause_pos);
        return;
    }
    size_t t;
    int64_t pos;
    if (!heard(p, &t, &pos)) return;
    timing(p, t, &p->pause_rate, &p->pause_length);
    p->paused = true;
    p->pause_track = t;
    p->pause_pos = pos;
    if (p->s->pause_mode == PAUSE_DEVICE && out_pause(p->out, true) == 0) {
        p->device_paused = true;
        return;
    }
    stop_output(p);
}

static void seek_by(player_t *p, int sec) {
    size_t t;
    int64_t pos;
    if (p->paused) {
        t = p->pause_track;
        pos = p->pause_pos;
    } else if (!heard(p, &t, &pos)) {
        return;
    }
    unsigned rate;
    int64_t length;
    timing(p, t, &rate, &length);
    if (!rate) return;
    int64_t target = pos + (int64_t)sec * rate;
    if (target < 0) target = 0;
    if (length >= 0 && target >= length) {
        jump(p, t + 1, 0);
        return;
    }
    if (p->paused && !p->device_paused) {
        p->pause_pos = target;
        return;
    }
    jump(p, t, target);
}

static void current(const player_t *p, size_t *t, int64_t *pos) {
    if (p->paused) {
        *t = p->pause_track;
        *pos = p->pause_pos;
    } else if (!heard(p, t, pos)) {
        *t = p->cur;
        *pos = 0;
    }
}

static void list_tracks(const player_t *p) {
    size_t cur, t;
    int64_t pos;
    current(p, &cur, &pos);
    for (t = 0; t < p->pl->n; t++) {
        const track_t *tr = &p->pl->items[t];
        char *name = tr->title ? xstrdup(tr->title) : xstrdup(path_basename(tr->path));
        ui_msg("%s %3zu %s", t == cur ? ">" : " ", t + 1, name);
        free(name);
    }
}

static void show_info(const player_t *p) {
    size_t t;
    int64_t pos;
    current(p, &t, &pos);
    const track_t *tr = &p->pl->items[t];
    ui_msg("file: %s%s", tr->path,
           p->dec && t == p->cur && dec_in_memory(p->dec) ? "  (in memory)" : "");
    if (tr->cue_track) ui_msg("cue:  track %d", tr->cue_track);
    if (out_is_open(p->out)) ui_msg("output: %s", out_describe(p->out));
}

static void help_keys(void) {
    ui_msg("keys: space pause  n next  b back  left/right -/+seek  up/down -/+long seek  "
           "g goto  l list  i info  q quit");
}

// Returns false to quit.
static bool handle_key(player_t *p, int key) {
    if (p->goto_mode) {
        size_t len = strlen(p->goto_buf);
        if (key >= '0' && key <= '9' && len + 1 < sizeof(p->goto_buf)) {
            p->goto_buf[len] = (char)key;
            p->goto_buf[len + 1] = '\0';
        } else if (key == KEY_BACKSPACE && len) {
            p->goto_buf[len - 1] = '\0';
        } else if (key == KEY_ENTER) {
            p->goto_mode = false;
            long n = atol(p->goto_buf);
            if (n >= 1 && (size_t)n <= p->pl->n)
                jump(p, (size_t)n - 1, 0);
            else if (p->goto_buf[0])
                ui_msg("no track %s", p->goto_buf);
        } else if (key == KEY_ESC) {
            p->goto_mode = false;
        }
        return true;
    }
    size_t t;
    int64_t pos;
    switch (key) {
    case 'q':
    case 'Q':
        return false;
    case ' ':
        toggle_pause(p);
        break;
    case 'n':
        current(p, &t, &pos);
        if (t + 1 < p->pl->n) jump(p, t + 1, 0);
        else ui_msg("last track");
        break;
    case 'b':
        current(p, &t, &pos);
        {
            unsigned rate;
            int64_t length;
            timing(p, t, &rate, &length);
            if (pos >= (int64_t)PREV_RESTART_SEC * rate || t == 0)
                jump(p, t, 0);
            else
                jump(p, t - 1, 0);
        }
        break;
    case KEY_RIGHT: seek_by(p, (int)p->s->seek_step); break;
    case KEY_LEFT:  seek_by(p, -(int)p->s->seek_step); break;
    case KEY_UP:    seek_by(p, (int)p->s->seek_step_long); break;
    case KEY_DOWN:  seek_by(p, -(int)p->s->seek_step_long); break;
    case 'g':
        p->goto_mode = true;
        p->goto_buf[0] = '\0';
        break;
    case 'l': list_tracks(p); break;
    case 'i': show_info(p); break;
    case 'h':
    case '?': help_keys(); break;
    default: break;
    }
    return true;
}

static void update_status(player_t *p) {
    size_t t;
    int64_t pos;
    if (p->goto_mode) {
        ui_status("go to track (1-%zu): %s", p->pl->n, p->goto_buf);
        return;
    }
    if (!p->paused && heard(p, &t, &pos) && t != p->announced) {
        const mark_t *m = heard_mark(p, t);
        if (m) ui_msg("%s", m->announce);
        p->announced = t;
    }
    current(p, &t, &pos);
    unsigned rate;
    int64_t length;
    timing(p, t, &rate, &length);
    char a[16], b[16];
    fmt_time(a, sizeof(a), pos, rate);
    fmt_time(b, sizeof(b), length, rate);
    ui_status("[%zu/%zu] %s / %s%s", t + 1, p->pl->n, a, b, p->paused ? "  (paused)" : "");
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int play(player_t *p, size_t first) {
    if (!advance(p, first, false)) {
        ui_err("no playable track");
        return 1;
    }
    double last_status = 0;
    while (!quit_signal) {
        // Short waits while the output plays out, so the drain starts in time.
        bool idle = !p->dec || p->waiting;
        int key = ui_read_key(p->paused ? 200 : idle ? 20 : 0);
        if (key != KEY_NONE && !handle_key(p, key)) break;
        double now = now_sec();
        if (now - last_status >= 0.2) {
            update_status(p);
            last_status = now;
        }
        if (p->paused) continue;
        if (p->waiting) {
            if (out_draining_left(p->out) > 0) continue;
            p->waiting = false;
            if (!advance(p, p->cur, true)) close_decoder(p);
            continue;
        }
        if (!p->dec) {
            // Nothing left to decode; leave once the output has played out.
            if (out_draining_left(p->out) > 0) continue;
            ui_status("finishing");
            out_drain_close(p->out);
            break;
        }
        long want = CHUNK_FRAMES;
        if (p->cur_end >= 0) {
            int64_t left = p->cur_end - dec_pos(p->dec);
            if (left < want) want = (long)(left > 0 ? left : 0);
        }
        snd_pcm_format_t fmt = out_format(p->out);
        long n = want > 0 ? dec_read(p->dec, p->buf, want, fmt) : 0;
        if (n <= 0) {
            if (n < 0) ui_msg("stopping track %zu after a decode error", p->cur + 1);
            if (!advance(p, p->cur + 1, true)) close_decoder(p);
            continue;
        }
        if (out_write(p->out, p->buf, n) < 0) {
            // The device is gone, for example CamillaDSP exited. Pause where
            // the listener is so a key press retries from there.
            size_t t;
            int64_t pos, length;
            unsigned rate;
            if (!heard(p, &t, &pos)) { t = p->cur; pos = 0; }
            timing(p, t, &rate, &length);
            hold(p, t, pos, rate, length, "output failed");
            continue;
        }
        p->written += n;
    }
    stop_output(p);
    return 0;
}

static void usage(FILE *f) {
    fputs("usage: cmdplay [-s CONFIG] [-p PLAYLIST] [OPTIONS] [FILE|CUE ...]\n"
          "  -s, --settings FILE    settings file (default ~/.config/cmdplay/cmdplay.conf)\n"
          "  -p, --playlist FILE    playlist: one path per line, .cue and .m3u accepted\n"
          "  -d, --device NAME      ALSA device, overrides the settings file\n"
          "  -n, --track N          start at track N\n"
          "  -l, --list             print the playlist and exit\n"
          "  -K, --no-keys          ignore the keyboard\n"
          "  -v, --verbose          show details\n"
          "  -h, --help\n", f);
}

int main(int argc, char **argv) {
    static const struct option opts[] = {
        {"settings", required_argument, NULL, 's'},
        {"playlist", required_argument, NULL, 'p'},
        {"device",   required_argument, NULL, 'd'},
        {"track",    required_argument, NULL, 'n'},
        {"list",     no_argument,       NULL, 'l'},
        {"no-keys",  no_argument,       NULL, 'K'},
        {"verbose",  no_argument,       NULL, 'v'},
        {"help",     no_argument,       NULL, 'h'},
        {NULL,       0,                 NULL, 0  },
    };
    const char *settings_path = NULL, *playlist_path = NULL, *device = NULL;
    long first = 1;
    bool list_only = false, no_keys = false;
    int c;
    while ((c = getopt_long(argc, argv, "s:p:d:n:lvKh", opts, NULL)) != -1) {
        switch (c) {
        case 's': settings_path = optarg; break;
        case 'p': playlist_path = optarg; break;
        case 'd': device = optarg; break;
        case 'n': first = atol(optarg); break;
        case 'l': list_only = true; break;
        case 'K': no_keys = true; break;
        case 'v': ui_verbose = 1; break;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 2;
        }
    }
    if (!playlist_path && optind >= argc) {
        usage(stderr);
        return 2;
    }

    settings_t s;
    settings_defaults(&s);
    char *default_path = settings_path ? NULL : settings_default_path();
    if (settings_path || default_path) {
        if (settings_load(&s, settings_path ? settings_path : default_path) < 0) return 1;
    }
    free(default_path);
    if (device) {
        free(s.device);
        s.device = xstrdup(device);
    }

    playlist_t pl = {0};
    if (playlist_path) {
        char *pp = path_resolve(playlist_path, NULL);
        int r = playlist_load(&pl, pp, s.text_encodings);
        free(pp);
        if (r < 0) return 1;
    }
    for (int i = optind; i < argc; i++) playlist_add(&pl, argv[i], s.text_encodings);
    if (pl.n == 0) {
        ui_err("the playlist is empty");
        return 1;
    }

    if (list_only) {
        for (size_t i = 0; i < pl.n; i++) {
            const track_t *t = &pl.items[i];
            if (t->cue_track) {
                char a[16];
                fmt_time(a, sizeof(a), t->start_cd, CD_FRAMES_PER_SEC);
                printf("%3zu %s [cue %02d @ %s] %s%s%s\n", i + 1, t->path, t->cue_track, a,
                       t->artist ? t->artist : "", t->artist ? " - " : "",
                       t->title ? t->title : "");
            } else {
                printf("%3zu %s\n", i + 1, t->path);
            }
        }
        playlist_free(&pl);
        settings_free(&s);
        return 0;
    }
    if (first < 1 || (size_t)first > pl.n) {
        ui_err("track %ld is not in the playlist (1-%zu)", first, pl.n);
        return 2;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    dec_global_init();
    output_t *out = out_new(&s);
    if (!out) return 1;
    ui_init(s.keys && !no_keys);
    ui_msg("cmdplay: %zu tracks, output %s%s%s", pl.n, s.device,
           s.alsa_conf ? " from " : "", s.alsa_conf ? s.alsa_conf : "");
    if (ui_keys_enabled()) help_keys();

    player_t p = { .s = &s, .pl = &pl, .out = out, .announced = (size_t)-1 };
    int rc = play(&p, (size_t)first - 1);

    close_decoder(&p);
    preload_cancel(p.next);
    clear_marks(&p);
    out_free(out);
    free(p.buf);
    ui_restore();
    playlist_free(&pl);
    settings_free(&s);
    return rc;
}