#define _POSIX_C_SOURCE 200809L
#include "decoder.h"
#include "mp4lite.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

struct Decoder {
    AVFormatContext *fmt;
    AVCodecContext *codec;
    SwrContext *swr;
    AVPacket *pkt;
    AVFrame *frame;
    int stream_idx;
    AVRational time_base;
    int64_t duration_ms;
    int64_t current_pts_ms;
    int64_t frames_since_pts;
    int64_t discard_until_ms;
    bool eof_reached;
    bool decoder_flushed;

    /* MP4/M4B audio via the low-memory reader instead of libavformat (mp4lite.h). */
    Mp4Lite *mp4;

    /* Input format the resampler was built for; rebuilt if frames change it (HE-AAC
     * reports its doubled SBR rate only once the first frame is decoded). */
    int swr_in_rate;
    int swr_in_fmt;
    int swr_in_channels;

    /* Leftover resampled S16LE stereo frames from previous decode call. */
    int16_t *fifo_buf;
    int fifo_cap_frames;
    int fifo_len_frames;
    int fifo_read_frames;
};

static void silence_ffmpeg_logs(void) {
    static bool initialized = false;
    if (!initialized) {
        av_log_set_level(AV_LOG_ERROR);
        initialized = true;
    }
}

static void copy_dict_tag(AVDictionary *dict, const char *key, char *dst, size_t cap) {
    if (!dict || !dst || cap == 0) return;
    AVDictionaryEntry *e = av_dict_get(dict, key, NULL, 0);
    if (e && e->value && e->value[0] != '\0') {
        snprintf(dst, cap, "%s", e->value);
    }
}

static int parse_track_number(const char *s) {
    if (!s || !*s) return 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (v > 0 && v < 100000) {
        return (int)v;
    }
    return 0;
}

int decoder_probe(const char *path, DecoderProbeInfo *info,
                  uint8_t **cover_out, size_t *cover_len) {
    if (!path || !info) {
        return -EINVAL;
    }
    silence_ffmpeg_logs();
    memset(info, 0, sizeof(*info));
    if (cover_out) *cover_out = NULL;
    if (cover_len) *cover_len = 0;

    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        return -ENOENT;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -EINVAL;
    }

    int audio_idx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVStream *st = fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            !(st->disposition & AV_DISPOSITION_ATTACHED_PIC) && audio_idx < 0) {
            audio_idx = (int)i;
        }
        if ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) &&
            st->attached_pic.data && st->attached_pic.size > 0) {
            info->has_cover = true;
            if (cover_out && cover_len && !*cover_out) {
                uint8_t *buf = malloc((size_t)st->attached_pic.size);
                if (buf) {
                    memcpy(buf, st->attached_pic.data, (size_t)st->attached_pic.size);
                    *cover_out = buf;
                    *cover_len = (size_t)st->attached_pic.size;
                }
            }
        }
    }

    if (audio_idx < 0) {
        avformat_close_input(&fmt);
        return -EINVAL;
    }

    AVStream *ast = fmt->streams[audio_idx];
    copy_dict_tag(fmt->metadata, "title", info->title, sizeof(info->title));
    copy_dict_tag(fmt->metadata, "album", info->album, sizeof(info->album));
    copy_dict_tag(fmt->metadata, "artist", info->artist, sizeof(info->artist));
    copy_dict_tag(fmt->metadata, "album_artist", info->album_artist, sizeof(info->album_artist));

    /* Ogg/Opus stores Vorbis comments on the audio stream rather than container metadata. */
    if (!info->title[0]) copy_dict_tag(ast->metadata, "title", info->title, sizeof(info->title));
    if (!info->album[0]) copy_dict_tag(ast->metadata, "album", info->album, sizeof(info->album));
    if (!info->artist[0]) copy_dict_tag(ast->metadata, "artist", info->artist, sizeof(info->artist));
    if (!info->album_artist[0]) copy_dict_tag(ast->metadata, "album_artist", info->album_artist, sizeof(info->album_artist));

    char track_str[64] = {0};
    copy_dict_tag(fmt->metadata, "track", track_str, sizeof(track_str));
    if (!track_str[0]) copy_dict_tag(ast->metadata, "track", track_str, sizeof(track_str));
    if (!track_str[0]) copy_dict_tag(ast->metadata, "tracknumber", track_str, sizeof(track_str));
    info->track_number = parse_track_number(track_str);

    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
        info->duration_ms = av_rescale_q(fmt->duration, (AVRational){1, AV_TIME_BASE}, (AVRational){1, 1000});
    } else if (ast->duration != AV_NOPTS_VALUE && ast->duration > 0) {
        info->duration_ms = av_rescale_q(ast->duration, ast->time_base, (AVRational){1, 1000});
    }

    /* Extract embedded chapters (M4B chpl/QuickTime chapter track, MP3 ID3 CHAP, Matroska/Ogg). */
    unsigned nb_ch = fmt->nb_chapters;
    if (nb_ch > DECODER_MAX_CHAPTERS) {
        nb_ch = DECODER_MAX_CHAPTERS;
    }
    for (unsigned i = 0; i < nb_ch; i++) {
        AVChapter *ch = fmt->chapters[i];
        DecoderChapter *out_ch = &info->chapters[info->chapter_count++];
        int64_t s_ms = av_rescale_q(ch->start, ch->time_base, (AVRational){1, 1000});
        int64_t e_ms = av_rescale_q(ch->end, ch->time_base, (AVRational){1, 1000});
        if (s_ms < 0) s_ms = 0;
        if (e_ms < s_ms) e_ms = (info->duration_ms > s_ms) ? info->duration_ms : s_ms;
        out_ch->start_ms = s_ms;
        out_ch->end_ms = e_ms;
        copy_dict_tag(ch->metadata, "title", out_ch->title, sizeof(out_ch->title));
        if (out_ch->title[0] == '\0') {
            snprintf(out_ch->title, sizeof(out_ch->title), "Chapter %u", i + 1);
        }
    }

    avformat_close_input(&fmt);
    return 0;
}

static int init_resampler_for(Decoder *d, const AVChannelLayout *layout, int fmt, int rate) {
    if (d->swr) {
        swr_free(&d->swr);
    }
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout in_layout;
    if (!layout || layout->nb_channels <= 0 ||
        av_channel_layout_copy(&in_layout, layout) < 0) {
        av_channel_layout_default(&in_layout, 2);
    }
    if (rate <= 0) rate = DECODER_OUT_RATE;

    int rc = swr_alloc_set_opts2(&d->swr,
                                 &out_layout, AV_SAMPLE_FMT_S16, DECODER_OUT_RATE,
                                 &in_layout, (enum AVSampleFormat)fmt, rate,
                                 0, NULL);
    d->swr_in_rate = rate;
    d->swr_in_fmt = fmt;
    d->swr_in_channels = in_layout.nb_channels;
    av_channel_layout_uninit(&in_layout);
    if (rc < 0 || !d->swr) {
        return -EINVAL;
    }
    if (swr_init(d->swr) < 0) {
        swr_free(&d->swr);
        return -EINVAL;
    }
    return 0;
}

static int init_resampler(Decoder *d) {
    return init_resampler_for(d, &d->codec->ch_layout, d->codec->sample_fmt,
                              d->codec->sample_rate);
}

/* AAC from an MP4/M4B through mp4lite: same decoder, a fraction of the memory. */
static int open_codec_mp4lite(Decoder *d, const char *path) {
    Mp4LiteInfo mi;
    if (mp4lite_open(&d->mp4, path, &mi) < 0) {
        return -ENOTSUP;
    }
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    d->codec = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!d->codec) {
        return -ENOTSUP;
    }
    d->codec->extradata = av_mallocz(mi.asc_len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!d->codec->extradata) {
        return -ENOMEM;
    }
    memcpy(d->codec->extradata, mi.asc, mi.asc_len);
    d->codec->extradata_size = (int)mi.asc_len;
    d->codec->sample_rate = mi.sample_rate;
    av_channel_layout_default(&d->codec->ch_layout, mi.channels > 0 ? mi.channels : 2);
    d->time_base = (AVRational){1, (int)mi.timescale};
    d->codec->pkt_timebase = d->time_base;
    d->duration_ms = av_rescale_q(mi.duration, d->time_base, (AVRational){1, 1000});
    if (avcodec_open2(d->codec, codec, NULL) < 0) {
        return -EINVAL;
    }
    d->stream_idx = 0;
    return 0;
}

static int open_codec_avformat(Decoder *d, const char *path) {
    if (avformat_open_input(&d->fmt, path, NULL, NULL) < 0) {
        return -ENOENT;
    }
    if (avformat_find_stream_info(d->fmt, NULL) < 0) {
        return -EINVAL;
    }

    for (unsigned i = 0; i < d->fmt->nb_streams; i++) {
        AVStream *st = d->fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            !(st->disposition & AV_DISPOSITION_ATTACHED_PIC) && d->stream_idx < 0) {
            d->stream_idx = (int)i;
        } else {
            /* Never read cover art or chapter-text packets during playback. */
            st->discard = AVDISCARD_ALL;
        }
    }
    if (d->stream_idx < 0) {
        return -EINVAL;
    }

    AVStream *st = d->fmt->streams[d->stream_idx];
    d->time_base = st->time_base;
    if (d->fmt->duration != AV_NOPTS_VALUE && d->fmt->duration > 0) {
        d->duration_ms = av_rescale_q(d->fmt->duration, (AVRational){1, AV_TIME_BASE}, (AVRational){1, 1000});
    } else if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
        d->duration_ms = av_rescale_q(st->duration, st->time_base, (AVRational){1, 1000});
    }

    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        return -ENOTSUP;
    }
    d->codec = avcodec_alloc_context3(codec);
    if (!d->codec) {
        return -ENOMEM;
    }
    if (avcodec_parameters_to_context(d->codec, st->codecpar) < 0) {
        return -EINVAL;
    }
    d->codec->pkt_timebase = st->time_base;
    if (avcodec_open2(d->codec, codec, NULL) < 0) {
        return -EINVAL;
    }
    return 0;
}

static Decoder *decoder_alloc(void) {
    Decoder *d = calloc(1, sizeof(*d));
    if (d) {
        d->stream_idx = -1;
        d->discard_until_ms = -1;
    }
    return d;
}

static void decoder_release_codec(Decoder *d) {
    if (d->codec) avcodec_free_context(&d->codec);
    if (d->fmt) avformat_close_input(&d->fmt);
    mp4lite_close(d->mp4);
    d->mp4 = NULL;
    d->stream_idx = -1;
    d->duration_ms = 0;
}

int decoder_open_ex(Decoder **out, const char *path, unsigned flags) {
    if (!out || !path) {
        return -EINVAL;
    }
    *out = NULL;
    silence_ffmpeg_logs();

    Decoder *d = decoder_alloc();
    if (!d) {
        return -ENOMEM;
    }

    const char *env = getenv("ALOUD_NO_MP4LITE");
    bool try_lite = !(flags & DECODER_FLAG_NO_MP4LITE) && !(env && env[0] == '1');
    int rc = try_lite ? open_codec_mp4lite(d, path) : -ENOTSUP;
    if (rc < 0) {
        decoder_release_codec(d);
        rc = open_codec_avformat(d, path);
    }
    if (rc < 0) {
        decoder_close(d);
        return rc;
    }

    if (init_resampler(d) < 0) {
        decoder_close(d);
        return -EINVAL;
    }

    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->fifo_cap_frames = 16384;
    d->fifo_buf = malloc((size_t)d->fifo_cap_frames * DECODER_OUT_CHANNELS * sizeof(int16_t));
    if (!d->pkt || !d->frame || !d->fifo_buf) {
        decoder_close(d);
        return -ENOMEM;
    }

    *out = d;
    return 0;
}

int decoder_open(Decoder **out, const char *path) {
    return decoder_open_ex(out, path, 0);
}

bool decoder_uses_mp4lite(const Decoder *d) {
    return d && d->mp4 != NULL;
}

void decoder_close(Decoder *d) {
    if (!d) return;
    free(d->fifo_buf);
    if (d->frame) av_frame_free(&d->frame);
    if (d->pkt) av_packet_free(&d->pkt);
    if (d->swr) swr_free(&d->swr);
    decoder_release_codec(d);
    free(d);
}

static int ensure_fifo_space(Decoder *d, int extra_frames) {
    if (d->fifo_read_frames > 0) {
        int rem = d->fifo_len_frames - d->fifo_read_frames;
        if (rem > 0) {
            memmove(d->fifo_buf,
                    d->fifo_buf + (size_t)d->fifo_read_frames * DECODER_OUT_CHANNELS,
                    (size_t)rem * DECODER_OUT_CHANNELS * sizeof(int16_t));
        }
        d->fifo_len_frames = rem;
        d->fifo_read_frames = 0;
    }
    int needed = d->fifo_len_frames + extra_frames;
    if (needed <= d->fifo_cap_frames) {
        return 0;
    }
    int new_cap = d->fifo_cap_frames * 2;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    int16_t *nbuf = realloc(d->fifo_buf, (size_t)new_cap * DECODER_OUT_CHANNELS * sizeof(int16_t));
    if (!nbuf) {
        return -ENOMEM;
    }
    d->fifo_buf = nbuf;
    d->fifo_cap_frames = new_cap;
    return 0;
}

static int push_decoded_frame(Decoder *d, const AVFrame *frame) {
    int64_t pts = frame->best_effort_timestamp;
    if (pts != AV_NOPTS_VALUE) {
        int64_t ms = av_rescale_q(pts, d->time_base, (AVRational){1, 1000});
        if (ms >= 0) {
            d->current_pts_ms = ms;
            d->frames_since_pts = 0;
        }
    }

    if (frame->sample_rate <= 0 || frame->ch_layout.nb_channels <= 0 || frame->nb_samples <= 0) {
        return 0; /* damaged frame: drop it, keep playing */
    }
    if (!d->swr || frame->sample_rate != d->swr_in_rate || frame->format != d->swr_in_fmt ||
        frame->ch_layout.nb_channels != d->swr_in_channels) {
        if (init_resampler_for(d, &frame->ch_layout, frame->format, frame->sample_rate) < 0) {
            return -EINVAL;
        }
    }

    int max_out = swr_get_out_samples(d->swr, frame->nb_samples);
    if (max_out <= 0) {
        max_out = frame->nb_samples * 2 + 256;
    }
    if (ensure_fifo_space(d, max_out) < 0) {
        return -ENOMEM;
    }

    uint8_t *out_ptr[1] = {
        (uint8_t *)(d->fifo_buf + (size_t)d->fifo_len_frames * DECODER_OUT_CHANNELS)
    };
    int converted = swr_convert(d->swr, out_ptr, max_out,
                                (const uint8_t **)frame->extended_data, frame->nb_samples);
    if (converted < 0) {
        return -EIO;
    }

    if (d->discard_until_ms >= 0) {
        int64_t cur_ms = decoder_position_ms(d);
        int64_t diff_ms = d->discard_until_ms - cur_ms;
        if (diff_ms > 3000) {
            /* Container seek landed far before target (sparse index): start from where it
             * landed rather than burning seconds of Cortex-A8 CPU on discarded audio. Short
             * gaps (MP3/AAC frame granularity) are still trimmed for a sample-exact seek. */
            d->discard_until_ms = -1;
        } else if (diff_ms > 0) {
            int64_t skip_frames = (diff_ms * DECODER_OUT_RATE) / 1000;
            if (skip_frames >= converted) {
                d->frames_since_pts += converted;
                return 0;
            } else if (skip_frames > 0) {
                memmove(out_ptr[0],
                        out_ptr[0] + (size_t)skip_frames * DECODER_OUT_CHANNELS * sizeof(int16_t),
                        (size_t)(converted - skip_frames) * DECODER_OUT_CHANNELS * sizeof(int16_t));
                converted -= (int)skip_frames;
                d->frames_since_pts += skip_frames;
                d->discard_until_ms = -1;
            }
        } else {
            d->discard_until_ms = -1;
        }
    }

    d->fifo_len_frames += converted;
    return 0;
}

/* Next packet of the chosen stream into d->pkt: 0 on success, AVERROR on end/error. */
static int read_audio_packet(Decoder *d) {
    if (!d->mp4) {
        return av_read_frame(d->fmt, d->pkt);
    }
    uint32_t size = 0;
    int64_t pts = 0;
    if (mp4lite_peek(d->mp4, &size, &pts) <= 0 || size == 0 || size > INT32_MAX) {
        return AVERROR_EOF;
    }
    if (av_new_packet(d->pkt, (int)size) < 0) {
        return AVERROR(ENOMEM);
    }
    if (mp4lite_read(d->mp4, d->pkt->data, size) < 0) {
        av_packet_unref(d->pkt);
        return AVERROR_EOF; /* truncated file: play what we have, then stop cleanly */
    }
    d->pkt->pts = pts;
    d->pkt->dts = pts;
    d->pkt->stream_index = 0;
    if (pts < 0 && d->codec->sample_rate > 0) {
        /* Encoder priming before the edit-list start: have the decoder drop it, exactly
         * as libavformat does with the same edit list. */
        int64_t skip = av_rescale(-pts, d->codec->sample_rate, d->time_base.den);
        uint8_t *sd = av_packet_new_side_data(d->pkt, AV_PKT_DATA_SKIP_SAMPLES, 10);
        if (sd && skip > 0 && skip < INT32_MAX) {
            AV_WL32(sd, (uint32_t)skip);
        }
    }
    return 0;
}

int decoder_read_frames(Decoder *d, int16_t *pcm_out, int max_frames) {
    if (!d || !pcm_out || max_frames <= 0) {
        return -EINVAL;
    }

    int written = 0;
    while (written < max_frames) {
        int avail = d->fifo_len_frames - d->fifo_read_frames;
        if (avail > 0) {
            int take = max_frames - written;
            if (take > avail) take = avail;
            memcpy(pcm_out + (size_t)written * DECODER_OUT_CHANNELS,
                   d->fifo_buf + (size_t)d->fifo_read_frames * DECODER_OUT_CHANNELS,
                   (size_t)take * DECODER_OUT_CHANNELS * sizeof(int16_t));
            d->fifo_read_frames += take;
            d->frames_since_pts += take;
            written += take;
            continue;
        }

        if (d->decoder_flushed) {
            break;
        }

        int rc = avcodec_receive_frame(d->codec, d->frame);
        if (rc == 0) {
            int prc = push_decoded_frame(d, d->frame);
            av_frame_unref(d->frame);
            if (prc < 0) {
                return written > 0 ? written : prc;
            }
            continue;
        } else if (rc == AVERROR_EOF) {
            /* Drain any buffered resampler samples. */
            int delay = d->swr ? swr_get_out_samples(d->swr, 0) : 0;
            if (delay > 0 && ensure_fifo_space(d, delay) == 0) {
                uint8_t *out_ptr[1] = {
                    (uint8_t *)(d->fifo_buf + (size_t)d->fifo_len_frames * DECODER_OUT_CHANNELS)
                };
                int conv = swr_convert(d->swr, out_ptr, delay, NULL, 0);
                if (conv > 0) {
                    d->fifo_len_frames += conv;
                }
            }
            d->decoder_flushed = true;
            continue;
        } else if (rc != AVERROR(EAGAIN)) {
            /* Corrupt frame in stream: skip packet and keep trying rather than crashing. */
            if (d->eof_reached) {
                d->decoder_flushed = true;
                break;
            }
        }

        if (d->eof_reached) {
            avcodec_send_packet(d->codec, NULL);
            continue;
        }

        int rrc = read_audio_packet(d);
        if (rrc < 0) {
            d->eof_reached = true;
            avcodec_send_packet(d->codec, NULL);
            continue;
        }

        if (d->pkt->stream_index == d->stream_idx) {
            /* Ignore corrupt packet errors so truncated/damaged files degrade gracefully. */
            (void)avcodec_send_packet(d->codec, d->pkt);
        }
        av_packet_unref(d->pkt);
    }

    if (written > 0 && d->duration_ms < decoder_position_ms(d)) {
        d->duration_ms = decoder_position_ms(d);
    }
    return written;
}

int decoder_seek_ms(Decoder *d, int64_t target_ms) {
    if (!d) {
        return -EINVAL;
    }
    if (target_ms < 0) {
        target_ms = 0;
    }
    if (d->duration_ms > 0 && target_ms > d->duration_ms) {
        target_ms = d->duration_ms;
    }

    int64_t target_ts = av_rescale_q(target_ms, (AVRational){1, 1000}, d->time_base);
    if (d->mp4) {
        if (mp4lite_seek(d->mp4, target_ts, NULL) < 0) {
            return -EIO;
        }
    } else {
        int rc = av_seek_frame(d->fmt, d->stream_idx, target_ts, AVSEEK_FLAG_BACKWARD);
        if (rc < 0) {
            rc = av_seek_frame(d->fmt, d->stream_idx, target_ts, AVSEEK_FLAG_ANY);
            if (rc < 0) {
                return -EIO;
            }
        }
    }

    avcodec_flush_buffers(d->codec);
    if (d->swr) {
        /* Drain resampler delay in-place instead of reallocating SwrContext on every seek. */
        uint8_t *out_ptr[1] = {(uint8_t *)d->fifo_buf};
        while (swr_get_out_samples(d->swr, 0) > 0) {
            if (swr_convert(d->swr, out_ptr, d->fifo_cap_frames, NULL, 0) <= 0) break;
        }
    }
    d->fifo_len_frames = 0;
    d->fifo_read_frames = 0;
    d->eof_reached = false;
    d->decoder_flushed = false;
    d->current_pts_ms = target_ms;
    d->frames_since_pts = 0;
    d->discard_until_ms = target_ms;
    return 0;
}

int64_t decoder_position_ms(const Decoder *d) {
    if (!d) return 0;
    int64_t delta_ms = (d->frames_since_pts * 1000) / DECODER_OUT_RATE;
    int64_t pos = d->current_pts_ms + delta_ms;
    if (pos < 0) pos = 0;
    if (d->duration_ms > 0 && pos > d->duration_ms) {
        pos = d->duration_ms;
    }
    return pos;
}

int64_t decoder_duration_ms(const Decoder *d) {
    return d ? d->duration_ms : 0;
}
