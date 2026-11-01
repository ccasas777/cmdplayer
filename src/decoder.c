#define _GNU_SOURCE
#include "decoder.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include "ui.h"
#include "util.h"

struct decoder {
    char *path;
    blob_t *mem;
    size_t mem_pos;
    AVIOContext *io;
    AVFormatContext *fmt;
    AVCodecContext *cc;
    AVStream *st;
    AVPacket *pkt;
    AVFrame *frame;
    int frame_off;
    bool have_frame;
    bool eof_sent;
    int64_t pos;
    // After a seek, samples before this frame are discarded; -1 when done.
    int64_t align_to;
    int64_t start_pts;
    dec_info_t info;
};

static const char *averr(int e) {
    static char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

static void av_log_to_ui(void *avcl, int level, const char *fmt, va_list vl) {
    if (level > (ui_verbose ? AV_LOG_WARNING : AV_LOG_ERROR)) return;
    static int print_prefix = 1;
    char line[1024];
    av_log_format_line(avcl, level, fmt, vl, line, sizeof(line), &print_prefix);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (n) ui_dbg("ffmpeg: %s", line);
}

void dec_global_init(void) {
    av_log_set_callback(av_log_to_ui);
}

static char *tag(const decoder_t *d, const char *key) {
    AVDictionaryEntry *e = av_dict_get(d->fmt->metadata, key, NULL, 0);
    if (!e) e = av_dict_get(d->st->metadata, key, NULL, 0);
    return e && e->value[0] ? xstrdup(e->value) : NULL;
}

static int mem_read(void *opaque, uint8_t *buf, int size) {
    decoder_t *d = opaque;
    size_t left = d->mem->size - d->mem_pos;
    if (left == 0) return AVERROR_EOF;
    size_t n = (size_t)size < left ? (size_t)size : left;
    memcpy(buf, d->mem->data + d->mem_pos, n);
    d->mem_pos += n;
    return (int)n;
}

static int64_t mem_seek(void *opaque, int64_t offset, int whence) {
    decoder_t *d = opaque;
    int64_t size = (int64_t)d->mem->size, to;
    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE: return size;
    case SEEK_SET: to = offset; break;
    case SEEK_CUR: to = (int64_t)d->mem_pos + offset; break;
    case SEEK_END: to = size + offset; break;
    default: return AVERROR(EINVAL);
    }
    if (to < 0 || to > size) return AVERROR(EINVAL);
    d->mem_pos = (size_t)to;
    return to;
}

decoder_t *dec_open(const char *path, blob_t *mem) {
    decoder_t *d = xcalloc(1, sizeof(*d));
    d->path = xstrdup(path);
    d->align_to = -1;
    d->mem = mem;
    if (mem) {
        enum { IO_BUF = 64 * 1024 };
        unsigned char *iobuf = av_malloc(IO_BUF);
        d->fmt = avformat_alloc_context();
        if (iobuf)
            d->io = avio_alloc_context(iobuf, IO_BUF, 0, d, mem_read, NULL, mem_seek);
        if (!d->io || !d->fmt) {
            if (!d->io) av_free(iobuf);
            ui_err("%s: out of memory", path);
            goto fail;
        }
        d->fmt->pb = d->io;
        d->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    int r = avformat_open_input(&d->fmt, path, NULL, NULL);
    if (r < 0) {
        ui_err("cannot open %s: %s", path, averr(r));
        goto fail;
    }
    if ((r = avformat_find_stream_info(d->fmt, NULL)) < 0) {
        ui_err("%s: %s", path, averr(r));
        goto fail;
    }
    const AVCodec *codec = NULL;
    int idx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (idx < 0 || !codec) {
        ui_err("%s: no playable audio stream", path);
        goto fail;
    }
    for (unsigned i = 0; i < d->fmt->nb_streams; i++)
        if ((int)i != idx) d->fmt->streams[i]->discard = AVDISCARD_ALL;
    d->st = d->fmt->streams[idx];
    d->cc = avcodec_alloc_context3(codec);
    if (!d->cc || avcodec_parameters_to_context(d->cc, d->st->codecpar) < 0) {
        ui_err("%s: cannot set up decoder", path);
        goto fail;
    }
    d->cc->pkt_timebase = d->st->time_base;
    if ((r = avcodec_open2(d->cc, codec, NULL)) < 0) {
        ui_err("%s: cannot open %s decoder: %s", path, codec->name, averr(r));
        goto fail;
    }
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->pkt || !d->frame) goto fail;

    dec_info_t *in = &d->info;
    in->rate = (unsigned)d->cc->sample_rate;
    in->channels = (unsigned)d->cc->ch_layout.nb_channels;
    if (in->rate == 0 || in->channels == 0) {
        ui_err("%s: unknown sample rate or channel count", path);
        goto fail;
    }
    snprintf(in->codec, sizeof(in->codec), "%s", codec->name);
    enum AVSampleFormat sf = av_get_packed_sample_fmt(d->cc->sample_fmt);
    int raw_bits = d->cc->bits_per_raw_sample;
    if (!raw_bits) raw_bits = d->st->codecpar->bits_per_raw_sample;
    switch (sf) {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_S16:
        in->native = SND_PCM_FORMAT_S16_LE;
        in->bits = sf == AV_SAMPLE_FMT_U8 ? 8 : 16;
        break;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S64:
        in->native = SND_PCM_FORMAT_S32_LE;
        in->bits = raw_bits > 0 && raw_bits <= 32 ? raw_bits : 32;
        break;
    case AV_SAMPLE_FMT_FLT:
        in->native = SND_PCM_FORMAT_FLOAT_LE;
        in->is_float = true;
        break;
    case AV_SAMPLE_FMT_DBL:
        in->native = SND_PCM_FORMAT_FLOAT64_LE;
        in->is_float = true;
        break;
    default:
        ui_err("%s: unsupported sample format %s", path,
               av_get_sample_fmt_name(d->cc->sample_fmt));
        goto fail;
    }
    d->start_pts = d->st->start_time != AV_NOPTS_VALUE ? d->st->start_time : 0;
    AVRational per_sample = {1, (int)in->rate};
    if (d->st->duration != AV_NOPTS_VALUE && d->st->duration > 0)
        in->frames = av_rescale_q(d->st->duration, d->st->time_base, per_sample);
    else if (d->fmt->duration != AV_NOPTS_VALUE && d->fmt->duration > 0)
        in->frames = av_rescale(d->fmt->duration, in->rate, AV_TIME_BASE);
    else
        in->frames = -1;
    in->title = tag(d, "title");
    in->artist = tag(d, "artist");
    in->album = tag(d, "album");
    return d;

fail:
    dec_close(d);
    return NULL;
}

const dec_info_t *dec_info(const decoder_t *d) { return &d->info; }
const char *dec_path(const decoder_t *d) { return d->path; }
int64_t dec_pos(const decoder_t *d) { return d->pos; }
bool dec_in_memory(const decoder_t *d) { return d->mem != NULL; }

void dec_close(decoder_t *d) {
    if (!d) return;
    av_frame_free(&d->frame);
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->cc);
    avformat_close_input(&d->fmt);
    if (d->io) {
        av_freep(&d->io->buffer);
        avio_context_free(&d->io);
    }
    blob_free(d->mem);
    free(d->info.title);
    free(d->info.artist);
    free(d->info.album);
    free(d->path);
    free(d);
}

int dec_seek(decoder_t *d, int64_t frame) {
    if (frame < 0) frame = 0;
    if (frame == d->pos) return 0;
    AVRational per_sample = {1, (int)d->info.rate};
    int64_t ts = av_rescale_q(frame, per_sample, d->st->time_base) + d->start_pts;
    // Files without a seek table can fail the timestamp search; the start
    // always works, and decoding from there up to frame is exact, just slow.
    int r = av_seek_frame(d->fmt, d->st->index, ts, AVSEEK_FLAG_BACKWARD);
    if (r < 0) r = avformat_seek_file(d->fmt, d->st->index, INT64_MIN, ts, ts, 0);
    if (r < 0) {
        ui_dbg("%s: seeking from the start", d->path);
        r = avformat_seek_file(d->fmt, d->st->index, INT64_MIN, d->start_pts, d->start_pts, 0);
    }
    if (r < 0) {
        ui_err("%s: seek failed: %s", d->path, averr(r));
        return -1;
    }
    avcodec_flush_buffers(d->cc);
    av_frame_unref(d->frame);
    d->have_frame = false;
    d->eof_sent = false;
    d->align_to = frame;
    d->pos = frame;
    return 0;
}

// 1 when d->frame holds samples, 0 at the end, < 0 on error.
static int next_frame(decoder_t *d) {
    for (;;) {
        int r = avcodec_receive_frame(d->cc, d->frame);
        if (r == 0) {
            if ((unsigned)d->frame->ch_layout.nb_channels != d->info.channels ||
                (unsigned)d->frame->sample_rate != d->info.rate) {
                ui_err("%s: format changes inside the file", d->path);
                av_frame_unref(d->frame);
                return -1;
            }
            d->frame_off = 0;
            if (d->align_to >= 0) {
                int64_t ts = d->frame->best_effort_timestamp;
                if (ts == AV_NOPTS_VALUE) ts = d->frame->pts;
                if (ts != AV_NOPTS_VALUE) {
                    AVRational per_sample = {1, (int)d->info.rate};
                    int64_t start = av_rescale_q(ts - d->start_pts, d->st->time_base, per_sample);
                    if (start + d->frame->nb_samples <= d->align_to) {
                        av_frame_unref(d->frame);
                        continue;
                    }
                    if (start < d->align_to)
                        d->frame_off = (int)(d->align_to - start);
                }
                d->align_to = -1;
            }
            d->have_frame = true;
            return 1;
        }
        if (r == AVERROR_EOF) return 0;
        if (r != AVERROR(EAGAIN)) {
            ui_err("%s: decode error: %s", d->path, averr(r));
            return r;
        }

        if (d->eof_sent) return 0;
        r = av_read_frame(d->fmt, d->pkt);
        if (r < 0) {
            if (r != AVERROR_EOF) ui_dbg("%s: read: %s", d->path, averr(r));
            avcodec_send_packet(d->cc, NULL);
            d->eof_sent = true;
            continue;
        }
        if (d->pkt->stream_index != d->st->index) {
            av_packet_unref(d->pkt);
            continue;
        }
        r = avcodec_send_packet(d->cc, d->pkt);
        av_packet_unref(d->pkt);
        if (r == AVERROR_INVALIDDATA) {
            ui_dbg("%s: skipped a damaged packet", d->path);
        } else if (r < 0 && r != AVERROR(EAGAIN)) {
            ui_err("%s: decode error: %s", d->path, averr(r));
            return r;
        }
    }
}

static inline int32_t clip_s32(double x) {
    double v = nearbyint(x * 2147483648.0);
    if (v >= 2147483647.0) return INT32_MAX;
    if (v <= -2147483648.0) return INT32_MIN;
    return (int32_t)v;
}

static inline int16_t clip_s16(double x) {
    double v = nearbyint(x * 32768.0);
    if (v >= 32767.0) return INT16_MAX;
    if (v <= -32768.0) return INT16_MIN;
    return (int16_t)v;
}

// Integer samples are carried left aligned in 32 bits, which keeps every
// integer source to S16/S32 output exact.
static void convert(const AVFrame *f, int off, int n, unsigned ch, void *out,
                    snd_pcm_format_t fmt) {
    enum AVSampleFormat sf = (enum AVSampleFormat)f->format;
    bool planar = av_sample_fmt_is_planar(sf);
    enum AVSampleFormat packed = av_get_packed_sample_fmt(sf);
    int bps = av_get_bytes_per_sample(sf);
    bool src_float = packed == AV_SAMPLE_FMT_FLT || packed == AV_SAMPLE_FMT_DBL;
    for (int i = 0; i < n; i++) {
        for (unsigned c = 0; c < ch; c++) {
            const uint8_t *p = planar
                ? f->extended_data[c] + (size_t)(off + i) * bps
                : f->extended_data[0] + ((size_t)(off + i) * ch + c) * bps;
            int32_t iv = 0;
            double fv = 0;
            switch (packed) {
            case AV_SAMPLE_FMT_U8:  iv = (int32_t)((uint32_t)(*p ^ 0x80) << 24); break;
            case AV_SAMPLE_FMT_S16: { int16_t v; memcpy(&v, p, 2); iv = (int32_t)((uint32_t)v << 16); break; }
            case AV_SAMPLE_FMT_S32: memcpy(&iv, p, 4); break;
            case AV_SAMPLE_FMT_S64: { int64_t v; memcpy(&v, p, 8); iv = (int32_t)(v >> 32); break; }
            case AV_SAMPLE_FMT_FLT: { float v; memcpy(&v, p, 4); fv = v; break; }
            case AV_SAMPLE_FMT_DBL: memcpy(&fv, p, 8); break;
            default: break;
            }
            size_t k = (size_t)i * ch + c;
            switch (fmt) {
            case SND_PCM_FORMAT_S16_LE:
                ((int16_t *)out)[k] = src_float ? clip_s16(fv) : (int16_t)(iv >> 16);
                break;
            case SND_PCM_FORMAT_S32_LE:
                ((int32_t *)out)[k] = src_float ? clip_s32(fv) : iv;
                break;
            case SND_PCM_FORMAT_FLOAT_LE:
                ((float *)out)[k] = src_float ? (float)fv : (float)(iv / 2147483648.0);
                break;
            case SND_PCM_FORMAT_FLOAT64_LE:
                ((double *)out)[k] = src_float ? fv : iv / 2147483648.0;
                break;
            default:
                break;
            }
        }
    }
}

long dec_read(decoder_t *d, void *buf, long max, snd_pcm_format_t fmt) {
    long got = 0;
    size_t frame_bytes = (size_t)snd_pcm_format_physical_width(fmt) / 8 * d->info.channels;
    while (got < max) {
        if (!d->have_frame) {
            int r = next_frame(d);
            if (r == 0) break;
            if (r < 0) return got ? got : r;
        }
        long avail = d->frame->nb_samples - d->frame_off;
        long n = avail < max - got ? avail : max - got;
        convert(d->frame, d->frame_off, (int)n, d->info.channels,
                (char *)buf + (size_t)got * frame_bytes, fmt);
        d->frame_off += (int)n;
        d->pos += n;
        got += n;
        if (d->frame_off >= d->frame->nb_samples) {
            av_frame_unref(d->frame);
            d->have_frame = false;
        }
    }
    return got;
}