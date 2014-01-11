/*
 * Copyright (c) 2013 Nicolas George
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/parseutils.h"
#include "avformat.h"
#include "internal.h"

enum SectionType {
    SEC_NONE = 0,
    SEC_FORMAT,
    SEC_STREAM,
    SEC_PACKET,
};

const char *const section_names[] = {
    [SEC_NONE]   = "NONE",
    [SEC_FORMAT] = "FORMAT",
    [SEC_STREAM] = "STREAM",
    [SEC_PACKET] = "PACKET",
};

typedef struct {
    AVClass *class;
    enum SectionType section;
    AVBPrint data;
    int packet_nb;
} FFprobeContext;

#define LINE_BUFFER_SIZE 4096

static int ffprobe_probe(AVProbeData *probe)
{
    unsigned score;

    if (!av_strstart(probe->buf, "[FORMAT]\n", NULL))
        return 0;
    score = !!strstr(probe->buf, "\nnb_streams=") +
            !!strstr(probe->buf, "\nformat_name=") +
            !!strstr(probe->buf, "\nfilename=");
    return score >= 2 ? AVPROBE_SCORE_MAX : AVPROBE_SCORE_MAX / 2;
}

static int ffprobe_read_close(AVFormatContext *avf)
{
    FFprobeContext *ffp = avf->priv_data;

    av_bprint_finalize(&ffp->data, NULL);
    return 0;
}

/**
 * Read a section start line ("[SECTION]").
 * Update FFprobeContext.section.
 * @return  SectionType (>0) for success,
 *          SEC_NONE if no section start,
 *          <0 for error
 */
static int read_section_start(AVFormatContext *avf)
{
    FFprobeContext *ffp = avf->priv_data;
    uint8_t buf[LINE_BUFFER_SIZE];
    const char *rest;
    int i, ret;

    ret = ff_get_line(avf->pb, buf, sizeof(buf));
    if (ret == 0 && avio_feof(avf->pb))
        ret = AVERROR_EOF;
    if (ret <= 0)
        return ret;
    if (*buf != '[')
        return 0;
    for (i = 1; i < FF_ARRAY_ELEMS(section_names); i++) {
        if (av_strstart(buf + 1, section_names[i], &rest) &&
            !strcmp(rest, "]\n")) {
            ffp->section = i;
            return i;
        }
    }
    return SEC_NONE;
}

/**
 * Read a line from within a section.
 * @return >0 for success, 0 if end of section, <0 for error
 */
static int read_section_line(AVFormatContext *avf, uint8_t *buf, size_t size)
{
    FFprobeContext *ffp = avf->priv_data;
    const char *rest;
    int ret, readlen;
    size_t l;

    ret = ff_get_line2(avf->pb, buf, size, &readlen);
    if (ret == 0) {
        av_log(avf, AV_LOG_ERROR, "Unterminated section, aborting\n");
        ret = AVERROR_INVALIDDATA;
        return ret;
    }
    if (readlen > ret) {
        av_log(avf, AV_LOG_ERROR,
               "Input read line was %d bytes long, maximum supported length is %zu\n",
               readlen, size-1);
        return AVERROR(EINVAL);
    }
    if (av_strstart(buf, "[/", &rest)) {
        ffp->section = 0;
        return 0;
    }
    if ((l = strlen(buf)) > 0 && buf[l - 1] == '\n')
        buf[--l] = 0;
    return 1;
}

/**
 * Read hexadecimal data.
 * Store it in FFprobeContext.data.
 * @return >=0 for success, <0 for error
 */
static int read_data(AVFormatContext *avf, int *size)
{
    FFprobeContext *ffp = avf->priv_data;
    uint8_t buf[LINE_BUFFER_SIZE], *cur;
    int ret, pos, val;

    if (ffp->data.len)
        return AVERROR_INVALIDDATA;
    while ((ret = read_section_line(avf, buf, sizeof(buf)))) {
        if (ret < 0)
            return ret;
        if (!buf[0])
            break;

        cur = buf;
        pos = 0;
        cur += pos;
        while (1) {
            while (*cur == ' ')
                cur++;
            if (!*cur)
                break;
            if ((unsigned)(*cur - '0') >= 10 &&
                (unsigned)(*cur - 'a') >=  6 &&
                (unsigned)(*cur - 'A') >=  6) {
                av_log(avf, AV_LOG_ERROR,
                       "Invalid character '%c' in packet number %d data\n", *cur, ffp->packet_nb);
                return AVERROR_INVALIDDATA;
            }
            pos = 0;
            if (sscanf(cur, " %02x%n", &val, &pos) < 1 || !pos) {
                av_log(avf, AV_LOG_ERROR,
                       "Could not parse value in packet number %d data\n", ffp->packet_nb);
                return AVERROR_INVALIDDATA;
            }
            cur += pos;
            av_bprint_chars(&ffp->data, val, 1);
        }
    }

    if (av_strstart(buf, "[/PACKET", NULL)) {
        avio_skip(avf->pb, -(strlen(buf)+1));
    }

    if (size)
        *size = ffp->data.len;

    return av_bprint_is_complete(&ffp->data) ? 0 : AVERROR(ENOMEM);
}

#define MAX_NB_STREAMS 32

static int read_section_format(AVFormatContext *avf)
{
    uint8_t buf[LINE_BUFFER_SIZE];
    int ret, val;

    while ((ret = read_section_line(avf, buf, sizeof(buf)))) {
        if (ret < 0)
            return ret;
        if (sscanf(buf, "nb_streams=%d", &val) >= 1) {
            if ((unsigned)val > MAX_NB_STREAMS) {
                av_log(avf, AV_LOG_ERROR, "Invalid streams number '%d', maximum allowed is %d\n",
                       val, MAX_NB_STREAMS);
                return AVERROR_INVALIDDATA;
            }
            while (avf->nb_streams < val)
                if (!avformat_new_stream(avf, NULL))
                    return AVERROR(ENOMEM);
        }
        /* TODO programs */
        /* TODO start_time duration bit_rate */
        /* TODO tags */
    }
    return SEC_FORMAT;
}

static int read_section_stream(AVFormatContext *avf)
{
    FFprobeContext *ffp = avf->priv_data;
    uint8_t buf[LINE_BUFFER_SIZE];
    int ret, index, val1, val2;
    AVStream *st = NULL;
    const char *val;

    av_bprint_clear(&ffp->data);
    while ((ret = read_section_line(avf, buf, sizeof(buf)))) {
        if (ret < 0)
            return ret;
        if (!st) {
            if (sscanf(buf, "index=%d", &index) >= 1) {
                if (index == avf->nb_streams) {
                    if (!avformat_new_stream(avf, NULL))
                        return AVERROR(ENOMEM);
                }
                if ((unsigned)index >= avf->nb_streams) {
                    av_log(avf, AV_LOG_ERROR, "Invalid stream index: %d\n",
                           index);
                    return AVERROR_INVALIDDATA;
                }
                st = avf->streams[index];
            } else {
                av_log(avf, AV_LOG_ERROR, "Stream without index\n");
                return AVERROR_INVALIDDATA;
            }
        }
        if (av_strstart(buf, "codec_name=", &val)) {
            const AVCodecDescriptor *desc = avcodec_descriptor_get_by_name(val);
            if (desc) {
                st->codecpar->codec_id   = desc->id;
                st->codecpar->codec_type = desc->type;
            }
            if (!desc) {
                av_log(avf, AV_LOG_WARNING, "Cannot recognize codec name '%s'", val);
            }
        } else if (!strcmp(buf, "extradata=")) {
            if ((ret = read_data(avf, NULL)) < 0)
                return ret;
            if (ffp->data.len) {
                av_freep(&st->codecpar->extradata);
                if ((ret = ff_alloc_extradata(st->codecpar, ffp->data.len)) < 0)
                    return ret;
                memcpy(st->codecpar->extradata, ffp->data.str, ffp->data.len);
            }
        } else if (sscanf(buf, "time_base=%d/%d", &val1, &val2) >= 2) {
            if (val1 <= 0 || val2 <= 0) {
                av_log(avf, AV_LOG_ERROR, "Invalid time base %d/%d\n",
                       val1, val2);
                return AVERROR_INVALIDDATA;
            }
            st->time_base.num = val1;
            st->time_base.den = val2;
        }
    }
    return SEC_STREAM;
}

static int read_section_packet(AVFormatContext *avf, AVPacket *pkt)
{
    FFprobeContext *ffp = avf->priv_data;
    uint8_t buf[LINE_BUFFER_SIZE];
    int ret;
    AVPacket p;
    char flags;
    int has_stream_index = 0;
    int has_pts_time = 0, has_dts_time = 0, has_duration_time = 0;
    int64_t pts = AV_NOPTS_VALUE, dts = AV_NOPTS_VALUE, duration = 0;

    av_init_packet(&p);
    p.pos = avio_tell(avf->pb);
    p.stream_index = -1;
    p.size = -1;
    av_bprint_clear(&ffp->data);

    while ((ret = read_section_line(avf, buf, sizeof(buf)))) {
        char timebuf[LINE_BUFFER_SIZE] = "";

        if (ret < 0)
            return ret;
        if (sscanf(buf, "stream_index=%d", &p.stream_index)) {
            if ((unsigned)p.stream_index >= avf->nb_streams) {
                av_log(avf, AV_LOG_ERROR, "Invalid stream number %d specified in packet number %d\n",
                       p.stream_index, ffp->packet_nb);
                return AVERROR_INVALIDDATA;
            }
            has_stream_index = 1;
        }

#define PARSE_TIME(name_, is_duration)                                  \
        sscanf(buf, #name_ "=%"SCNi64, &p.name_);                       \
        if (sscanf(buf, #name_ "_time=%s", timebuf) > 0) {              \
            has_##name_##_time = 1;                                     \
            if (!strcmp(timebuf, "N/A")) {                              \
                name_ = is_duration ? 0 : AV_NOPTS_VALUE;               \
            } else {                                                    \
                ret = av_parse_time(&name_, timebuf, 1);                \
                if (ret < 0) {                                          \
                    av_log(avf, AV_LOG_ERROR, "Invalid " #name_ " time specification '%s' for packet #%d data\n", \
                           timebuf, ffp->packet_nb);                    \
                    return ret;                                         \
                }                                                       \
            }                                                           \
        }                                                               \

        PARSE_TIME(pts, 0);
        PARSE_TIME(dts, 0);
        PARSE_TIME(duration, 1);

        if (sscanf(buf, "flags=%c", &flags) >= 1)
            p.flags = flags == 'K' ? AV_PKT_FLAG_KEY : 0;
        if (!strcmp(buf, "data="))
            if ((ret = read_data(avf, &p.size)) < 0)
                return ret;
    }

    if (!has_stream_index) {
        av_log(avf, AV_LOG_ERROR, "No stream index was specified for packet #%d, aborting\n", ffp->packet_nb);
        return AVERROR_INVALIDDATA;
    }

    if (p.size < 0 || (unsigned)p.stream_index >= avf->nb_streams)
        return SEC_NONE;

    if (has_pts_time && pts != AV_NOPTS_VALUE)
        p.pts = av_rescale_q(pts, AV_TIME_BASE_Q, avf->streams[p.stream_index]->time_base);
    if (has_dts_time && dts != AV_NOPTS_VALUE)
        p.dts = av_rescale_q(dts, AV_TIME_BASE_Q, avf->streams[p.stream_index]->time_base);
    if (has_duration_time && duration != 0)
        p.duration = av_rescale_q(duration, AV_TIME_BASE_Q, avf->streams[p.stream_index]->time_base);

    if ((ret = av_new_packet(pkt, p.size)) < 0)
        return ret;
    p.data = pkt->data;
    p.buf  = pkt->buf;
    *pkt = p;
    if (ffp->data.len) {
        ffp->data.len = FFMIN(ffp->data.len, pkt->size);
        memcpy(pkt->data, ffp->data.str, ffp->data.len);
    }
    return SEC_PACKET;
}

static int read_section(AVFormatContext *avf, AVPacket *pkt)
{
    FFprobeContext *ffp = avf->priv_data;
    int ret, section;

    while (!ffp->section)
        if ((ret = read_section_start(avf)) < 0)
            return ret;
    switch (section = ffp->section) {
    case SEC_FORMAT:
        return read_section_format(avf);
    case SEC_STREAM:
        return read_section_stream(avf);
    case SEC_PACKET:
        ret = read_section_packet(avf, pkt);
        ffp->packet_nb++;
        return ret;
    default:
        av_assert0(!"reached");
        return AVERROR_BUG;
    }
}

static int ffprobe_read_header(AVFormatContext *avf)
{
    FFprobeContext *ffp = avf->priv_data;
    int ret;
    int nb_streams = 0;

    if (avf->strict_std_compliance > FF_COMPLIANCE_EXPERIMENTAL) {
        av_log(avf, AV_LOG_ERROR, "The ffprobe demuxer is experimental and requires the strict option set to 'experimental'.\n");
        return AVERROR_EXPERIMENTAL;
    }

    av_bprint_init(&ffp->data, 0, AV_BPRINT_SIZE_UNLIMITED);
    if ((ret = read_section_start(avf)) < 0)
        return ret;
    if (ret != SEC_FORMAT) {
        av_log(avf, AV_LOG_INFO, "Using noheader mode\n");
        avf->ctx_flags |= AVFMTCTX_NOHEADER;
        return 0;
    }
    if ((ret = read_section_format(avf)) < 0)
        return ret;

    /* read stream information */
    while (1) {
        ret = read_section_start(avf);
        if (ret != SEC_STREAM)
            break;
        if ((ret = read_section_stream(avf)) < 0)
            return ret;
        nb_streams++;
    }

    if (nb_streams != avf->nb_streams) {
        av_log(avf, AV_LOG_ERROR, "Number of declared streams is %d, but only %d streams were specified in STREAM sections\n",
               avf->nb_streams, nb_streams);
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

static int ffprobe_read_packet(AVFormatContext *avf, AVPacket *pkt)
{
    int ret;

    while (1) {
        if ((ret = read_section(avf, pkt)) < 0)
            return ret;
        if (ret == SEC_PACKET)
            return 0;
    }
}

static const AVClass ffprobe_class = {
    .class_name = "ffprobe demuxer",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_ffprobe_demuxer = {
    .name           = "ffprobe",
    .long_name      = NULL_IF_CONFIG_SMALL("FFprobe alike output"),
    .priv_data_size = sizeof(FFprobeContext),
    .read_probe     = ffprobe_probe,
    .read_header    = ffprobe_read_header,
    .read_packet    = ffprobe_read_packet,
    .read_close     = ffprobe_read_close,
    .priv_class     = &ffprobe_class,
};
