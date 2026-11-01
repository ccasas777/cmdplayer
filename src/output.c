#define _GNU_SOURCE
#include "output.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "util.h"

struct output {
    const settings_t *s;
    snd_config_t *lconf;
    snd_pcm_t *pcm;
    // Stream as requested by the player.
    snd_pcm_format_t req_fmt;
    unsigned rate, src_ch;
    // As opened.
    snd_pcm_format_t fmt;
    unsigned dev_ch;
    bool interleaved;
    bool remap;
    int map[MAX_CHANNELS];
    size_t sample_bytes;
    snd_pcm_uframes_t period, buffer;
    bool can_pause;
    char *scratch;
    size_t scratch_size;
    char desc[200];
};

static void alsa_error(const char *file, int line, const char *func, int err,
                       const char *fmt, ...) {
    (void)file;
    (void)line;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (err)
        ui_msg("alsa: %s: %s (%s)", func, buf, snd_strerror(err));
    else
        ui_msg("alsa: %s: %s", func, buf);
}

// The system configuration with the file merged on top of it.
static snd_config_t *load_lconf(const char *path) {
    snd_config_t *top = NULL, *conf = NULL;
    snd_input_t *in = NULL;
    int r = snd_config_update_ref(&top);
    if (r >= 0) {
        r = snd_config_copy(&conf, top);
        snd_config_unref(top);
    }
    if (r >= 0) r = snd_input_stdio_open(&in, path, "r");
    if (r >= 0) {
        r = snd_config_load(conf, in);
        snd_input_close(in);
    }
    if (r < 0) {
        ui_err("cannot load ALSA config %s: %s", path, snd_strerror(r));
        if (conf) snd_config_delete(conf);
        return NULL;
    }
    return conf;
}

output_t *out_new(const settings_t *s) {
    snd_lib_error_set_handler(alsa_error);
    output_t *o = xcalloc(1, sizeof(*o));
    o->s = s;
    if (s->alsa_conf && !(o->lconf = load_lconf(s->alsa_conf))) {
        free(o);
        return NULL;
    }
    return o;
}

void out_free(output_t *o) {
    if (!o) return;
    out_drop_close(o);
    if (o->lconf) snd_config_delete(o->lconf);
    free(o->scratch);
    free(o);
}

bool out_is_open(const output_t *o) { return o->pcm != NULL; }
snd_pcm_format_t out_format(const output_t *o) { return o->fmt; }
const char *out_describe(const output_t *o) { return o->desc; }

bool out_matches(const output_t *o, snd_pcm_format_t fmt, unsigned rate, unsigned channels) {
    return o->pcm && o->req_fmt == fmt && o->rate == rate && o->src_ch == channels;
}

static void build_map(output_t *o) {
    const settings_t *s = o->s;
    o->remap = o->dev_ch != o->src_ch;
    for (unsigned k = 0; k < o->dev_ch; k++) {
        int src;
        if (k < s->n_channel_map)
            src = s->channel_map[k];
        else if (k < o->src_ch)
            src = (int)k;
        else if (o->src_ch == 1 && k < 2)
            src = 0;
        else
            src = -1;
        if (src >= (int)o->src_ch) src = -1;
        o->map[k] = src;
        if (src != (int)k) o->remap = true;
    }
}

static int setup(output_t *o, snd_pcm_format_t fmt, unsigned rate, unsigned channels,
                 bool with_times) {
    const settings_t *s = o->s;
    snd_pcm_t *pcm = o->pcm;
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    int r = snd_pcm_hw_params_any(pcm, hw);
    if (r < 0) return r;
    snd_pcm_hw_params_set_rate_resample(pcm, hw, s->allow_resample ? 1 : 0);

    o->interleaved = true;
    if (snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
        if ((r = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_NONINTERLEAVED)) < 0) {
            ui_err("%s: no read/write access mode", s->device);
            return r;
        }
        o->interleaved = false;
    }

    snd_pcm_format_t cands[2] = {fmt, SND_PCM_FORMAT_S32_LE};
    o->fmt = SND_PCM_FORMAT_UNKNOWN;
    for (int i = 0; i < 2; i++) {
        if (snd_pcm_hw_params_test_format(pcm, hw, cands[i]) == 0) {
            o->fmt = cands[i];
            break;
        }
    }
    if (o->fmt == SND_PCM_FORMAT_UNKNOWN) {
        ui_err("%s does not take %s or S32_LE", s->device, snd_pcm_format_name(fmt));
        return -EINVAL;
    }
    snd_pcm_hw_params_set_format(pcm, hw, o->fmt);

    unsigned dev_ch = s->channels ? s->channels : channels;
    if (snd_pcm_hw_params_set_channels(pcm, hw, dev_ch) < 0) {
        if (s->channels || snd_pcm_hw_params_set_channels_near(pcm, hw, &dev_ch) < 0) {
            ui_err("%s does not take %u channels", s->device, dev_ch);
            return -EINVAL;
        }
    }
    o->dev_ch = dev_ch;

    if (snd_pcm_hw_params_set_rate(pcm, hw, rate, 0) < 0) {
        ui_err("%s does not take %u Hz%s", s->device, rate,
               s->allow_resample ? "" : " (resampling is off)");
        return -EINVAL;
    }

    if (with_times) {
        unsigned bt = s->buffer_ms * 1000, pt = s->period_ms * 1000;
        if (bt) snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &bt, NULL);
        if (pt) snd_pcm_hw_params_set_period_time_near(pcm, hw, &pt, NULL);
    }
    if ((r = snd_pcm_hw_params(pcm, hw)) < 0) return r;
    snd_pcm_hw_params_get_period_size(hw, &o->period, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, &o->buffer);
    o->can_pause = snd_pcm_hw_params_can_pause(hw);

    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, o->buffer);
    snd_pcm_sw_params_set_avail_min(pcm, sw, o->period);
    if ((r = snd_pcm_sw_params(pcm, sw)) < 0) return r;
    return 0;
}

int out_open(output_t *o, snd_pcm_format_t fmt, unsigned rate, unsigned channels) {
    const settings_t *s = o->s;
    out_drop_close(o);
    int r = o->lconf
        ? snd_pcm_open_lconf(&o->pcm, s->device, SND_PCM_STREAM_PLAYBACK, 0, o->lconf)
        : snd_pcm_open(&o->pcm, s->device, SND_PCM_STREAM_PLAYBACK, 0);
    if (r < 0) {
        o->pcm = NULL;
        ui_err("cannot open %s: %s", s->device, snd_strerror(r));
        return r;
    }

    r = setup(o, fmt, rate, channels, true);
    if (r < 0 && r != -EINVAL) {
        ui_dbg("%s: retrying without buffer and period times", s->device);
        r = setup(o, fmt, rate, channels, false);
    }

    if (r < 0) {
        if (r != -EINVAL) ui_err("cannot set up %s: %s", s->device, snd_strerror(r));
        snd_pcm_close(o->pcm);
        o->pcm = NULL;
        return r;
    }

    o->req_fmt = fmt;
    o->rate = rate;
    o->src_ch = channels;
    o->sample_bytes = (size_t)snd_pcm_format_physical_width(o->fmt) / 8;
    build_map(o);
    size_t need = (size_t)o->period * o->dev_ch * o->sample_bytes;
    if (need > o->scratch_size) {
        o->scratch = xrealloc(o->scratch, need);
        o->scratch_size = need;
    }
    snprintf(o->desc, sizeof(o->desc), "%s: %s %u Hz %u ch%s, buffer %.0f ms, period %.0f ms",
             s->device, snd_pcm_format_name(o->fmt), rate, o->dev_ch,
             o->interleaved ? "" : " non-interleaved",
             o->buffer * 1000.0 / rate, o->period * 1000.0 / rate);
    ui_dbg("opened %s", o->desc);
    return 0;
}

// Copies n frames into the device layout in scratch.
static void layout(output_t *o, const char *src, long n) {
    size_t sb = o->sample_bytes;
    size_t src_frame = sb * o->src_ch;
    for (unsigned k = 0; k < o->dev_ch; k++) {
        int m = o->map[k];
        for (long i = 0; i < n; i++) {
            char *dst = o->interleaved
                ? o->scratch + ((size_t)i * o->dev_ch + k) * sb
                : o->scratch + ((size_t)k * (size_t)n + (size_t)i) * sb;
            if (m < 0)
                memset(dst, 0, sb);
            else
                memcpy(dst, src + (size_t)i * src_frame + (size_t)m * sb, sb);
        }
    }
}

int out_write(output_t *o, const void *buf, long frames) {
    if (!o->pcm) return -EBADFD;
    const char *p = buf;
    size_t src_frame = o->sample_bytes * o->src_ch;
    while (frames > 0) {
        long n = frames < (long)o->period ? frames : (long)o->period;
        snd_pcm_sframes_t r;
        if (o->interleaved && !o->remap) {
            r = snd_pcm_writei(o->pcm, p, (snd_pcm_uframes_t)n);
        } else {
            layout(o, p, n);
            if (o->interleaved) {
                r = snd_pcm_writei(o->pcm, o->scratch, (snd_pcm_uframes_t)n);
            } else {
                void *planes[MAX_CHANNELS];
                for (unsigned k = 0; k < o->dev_ch; k++)
                    planes[k] = o->scratch + (size_t)k * (size_t)n * o->sample_bytes;
                r = snd_pcm_writen(o->pcm, planes, (snd_pcm_uframes_t)n);
            }
        }
        if (r == -EAGAIN || r == -EINTR) continue;
        if (r < 0) {
            int rr = snd_pcm_recover(o->pcm, (int)r, 1);
            if (rr < 0) {
                ui_err("%s: %s", o->s->device, snd_strerror((int)r));
                return rr;
            }
            ui_dbg("%s: recovered from %s", o->s->device, snd_strerror((int)r));
            continue;
        }
        p += (size_t)r * src_frame;
        frames -= r;
    }
    return 0;
}

long out_delay(const output_t *o) {
    snd_pcm_sframes_t d = 0;
    if (!o->pcm || snd_pcm_delay(o->pcm, &d) < 0 || d < 0) return 0;
    return (long)d;
}

long out_draining_left(const output_t *o) {
    if (!o->pcm || snd_pcm_state(o->pcm) != SND_PCM_STATE_RUNNING) return 0;
    long d = out_delay(o);
    return d > 2 * (long)o->period ? d - 2 * (long)o->period : 0;
}

void out_drain_close(output_t *o) {
    if (!o->pcm) return;
    snd_pcm_drain(o->pcm);
    snd_pcm_close(o->pcm);
    o->pcm = NULL;
}

void out_drop_close(output_t *o) {
    if (!o->pcm) return;
    snd_pcm_drop(o->pcm);
    snd_pcm_close(o->pcm);
    o->pcm = NULL;
}

int out_pause(output_t *o, bool on) {
    if (!o->pcm || !o->can_pause) return -ENOSYS;
    int r = snd_pcm_pause(o->pcm, on ? 1 : 0);
    if (r < 0) ui_dbg("%s: pause: %s", o->s->device, snd_strerror(r));
    return r;
}