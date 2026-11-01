#define _GNU_SOURCE
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ui.h"
#include "util.h"

void settings_defaults(settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->device = xstrdup("default");
    s->sample_format = FMT_NATIVE;
    s->buffer_ms = 500;
    s->period_ms = 100;
    s->gapless = true;
    s->pause_mode = PAUSE_REOPEN;
    s->seek_step = 10;
    s->seek_step_long = 60;
    s->keys = true;
    s->text_encodings = xstrdup("BIG5,GB18030,SHIFT_JIS");
    s->preload_max_mb = 4096;
}

void settings_free(settings_t *s) {
    free(s->device);
    free(s->alsa_conf);
    free(s->text_encodings);
}

char *settings_default_path(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char *p;
    if (xdg && xdg[0])
        p = xasprintf("%s/cmdplay/cmdplay.conf", xdg);
    else if (home)
        p = xasprintf("%s/.config/cmdplay/cmdplay.conf", home);
    else
        return NULL;
    if (path_exists(p)) return p;
    free(p);
    return NULL;
}

static int parse_bool(const char *v, bool *out) {
    static const char *yes[] = { [0]="yes", [1]="true", [2]="on", [3]="1" };
    static const char *no[] = { [0]="no", [1]="false", [2]="off", [3]="0" };
    for (size_t i = 0; i < 4; i++) {
        if (strcasecmp(v, yes[i]) == 0) { *out = true; return 0; }
        if (strcasecmp(v, no[i]) == 0) { *out = false; return 0; }
    }
    return -1;
}

static int parse_uint(const char *v, unsigned max, unsigned *out) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(v, &end, 10);
    if (errno || end == v || *end || v[0] == '-' || n > max) return -1;
    *out = (unsigned)n;
    return 0;
}

static int parse_channel_map(const char *v, settings_t *s) {
    s->n_channel_map = 0;
    char *copy = xstrdup(v), *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        unsigned ch;
        bool silent = strcmp(tok, "-") == 0;
        if (s->n_channel_map >= MAX_CHANNELS || (!silent && parse_uint(tok, MAX_CHANNELS - 1, &ch))) {
            free(copy);
            return -1;
        }
        s->channel_map[s->n_channel_map++] = silent ? -1 : (int)ch;
    }
    free(copy);
    return 0;
}

// "370", "370 ms" or "16384 frames".
static int parse_latency(const char *v, settings_t *s) {
    char num[32];
    size_t n = strspn(v, "0123456789");
    if (n == 0 || n >= sizeof(num)) return -1;
    memcpy(num, v, n);
    num[n] = '\0';
    const char *unit = v + n;
    while (*unit == ' ' || *unit == '\t') unit++;
    s->latency_ms = s->latency_frames = 0;
    if (!*unit || strcasecmp(unit, "ms") == 0) return parse_uint(num, 60000, &s->latency_ms);
    if (strcasecmp(unit, "frames") == 0) return parse_uint(num, 10000000, &s->latency_frames);
    return -1;
}

// Value of a "key = value" line: quoted, or up to an inline "#" comment.
static char *parse_value(char *v) {
    v = str_trim(v);
    if (*v == '"') {
        char *end = strrchr(v + 1, '"');
        if (!end) return NULL;
        *end = '\0';
        return v + 1;
    }
    for (char *p = v; *p; p++) {
        if ((*p == '#' || *p == ';') && (p == v || p[-1] == ' ' || p[-1] == '\t')) {
            *p = '\0';
            break;
        }
    }
    return str_trim(v);
}

static int set_key(settings_t *s, const char *section, const char *key,
                   const char *val, const char *base_dir) {
    bool b;
    unsigned u;
    if (strcmp(section, "output") == 0) {
        if (strcmp(key, "device") == 0) {
            free(s->device);
            s->device = xstrdup(val);
            return 0;
        }
        if (strcmp(key, "alsa_conf") == 0) {
            free(s->alsa_conf);
            s->alsa_conf = val[0] ? path_resolve(val, base_dir) : NULL;
            return 0;
        }
        if (strcmp(key, "sample_format") == 0) {
            if (strcasecmp(val, "native") == 0) s->sample_format = FMT_NATIVE;
            else if (strcasecmp(val, "S32_LE") == 0) s->sample_format = FMT_S32;
            else return -1;
            return 0;
        }
        if (strcmp(key, "buffer_time") == 0) return parse_uint(val, 10000, &s->buffer_ms);
        if (strcmp(key, "period_time") == 0) return parse_uint(val, 5000, &s->period_ms);
        if (strcmp(key, "channels") == 0) return parse_uint(val, MAX_CHANNELS, &s->channels);
        if (strcmp(key, "channel_map") == 0) return parse_channel_map(val, s);
        if (strcmp(key, "allow_resample") == 0) {
            if (parse_bool(val, &b)) return -1;
            s->allow_resample = b;
            return 0;
        }
        if (strcmp(key, "latency") == 0) return parse_latency(val, s);
    } else if (strcmp(section, "player") == 0) {
        if (strcmp(key, "gapless") == 0) {
            if (parse_bool(val, &b)) return -1;
            s->gapless = b;
            return 0;
        }
        if (strcmp(key, "pause_mode") == 0) {
            if (strcasecmp(val, "reopen") == 0) s->pause_mode = PAUSE_REOPEN;
            else if (strcasecmp(val, "device") == 0) s->pause_mode = PAUSE_DEVICE;
            else return -1;
            return 0;
        }
        if (strcmp(key, "seek_step") == 0) {
            if (parse_uint(val, 3600, &u) || u == 0) return -1;
            s->seek_step = u;
            return 0;
        }
        if (strcmp(key, "seek_step_long") == 0) {
            if (parse_uint(val, 36000, &u) || u == 0) return -1;
            s->seek_step_long = u;
            return 0;
        }
        if (strcmp(key, "keys") == 0) {
            if (parse_bool(val, &b)) return -1;
            s->keys = b;
            return 0;
        }
        if (strcmp(key, "text_encodings") == 0) {
            free(s->text_encodings);
            s->text_encodings = xstrdup(val);
            return 0;
        }
        if (strcmp(key, "preload") == 0) {
            if (parse_bool(val, &b)) return -1;
            s->preload = b;
            return 0;
        }
        if (strcmp(key, "preload_max") == 0) {
            if (parse_uint(val, 1u << 20, &u) || u == 0) return -1;
            s->preload_max_mb = u;
            return 0;
        }
    }
    return -2;
}

int settings_load(settings_t *s, const char *path) {
    size_t len;
    char *raw = read_file(path, &len);
    if (!raw) {
        ui_err("cannot read config %s: %s", path, strerror(errno));
        return -1;
    }
    char *text = text_to_utf8(raw, len, NULL, NULL);
    free(raw);
    if (!text) {
        ui_err("config %s is not UTF-8", path);
        return -1;
    }
    char *base_dir = path_dirname(path);
    char section[64] = "";
    int rc = 0, lineno = 0;
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        lineno++;
        char *l = str_trim(line);
        if (!*l || *l == '#' || *l == ';') continue;
        if (*l == '[') {
            char *end = strchr(l, ']');
            if (!end || (size_t)(end - l - 1) >= sizeof(section)) {
                ui_err("%s:%d: bad section header", path, lineno);
                rc = -1;
                break;
            }
            *end = '\0';
            snprintf(section, sizeof(section), "%s", str_trim(l + 1));
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) {
            ui_err("%s:%d: expected key = value", path, lineno);
            rc = -1;
            break;
        }
        *eq = '\0';
        char *key = str_trim(l);
        char *val = parse_value(eq + 1);
        int r = val ? set_key(s, section, key, val, base_dir) : -1;
        if (r == -2) {
            ui_err("%s:%d: unknown setting [%s] %s", path, lineno, section, key);
            rc = -1;
            break;
        }
        if (r < 0) {
            ui_err("%s:%d: bad value for %s", path, lineno, key);
            rc = -1;
            break;
        }
    }
    free(base_dir);
    free(text);
    return rc;
}