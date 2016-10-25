/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * ffprobe format muxer
 */

#include "avformat.h"
#include "avio.h"

static int ffprobe_write_header(AVFormatContext *s)
{
    int i;

    avio_printf(s->pb, "[FORMAT]\n");
    avio_printf(s->pb, "nb_streams=%d\n", s->nb_streams);
    avio_printf(s->pb, "format_name=ffprobe\n");
    avio_printf(s->pb, "[/FORMAT]\n");

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        const AVCodecDescriptor *cd = avcodec_descriptor_get(st->codecpar->codec_id);

        avio_printf(s->pb, "[STREAM]\n");
        avio_printf(s->pb, "index=%d\n", i);
        avio_printf(s->pb, "codec_name=%s\n", cd->name);
        avio_printf(s->pb, "time_base=%d/%d\n", st->time_base.num, st->time_base.den);
        avio_printf(s->pb, "[/STREAM]\n");
    }

    return 0;
}

static void ffprobe_write_data(AVFormatContext *s, uint8_t *data, size_t data_size)
{
#define BYTES_NB 64
    int i, j;
    uint8_t buf[BYTES_NB * 2 + 2];

    avio_printf(s->pb, "data=\n");

    for (j = 0, i = 0; i < data_size; i++, j++) {
        sprintf(buf+2*j, "%02x", data[i]);
        if (j == BYTES_NB) {
            buf[j++] = '\n';
            buf[j++] = 0;
            avio_printf(s->pb, "%s", buf);
            j = 0;
        }
    }

    avio_printf(s->pb, "\n");
}

static int ffprobe_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVStream *st = s->streams[pkt->stream_index];
    const char *type;

    avio_printf(s->pb, "[PACKET]\n");
    type = av_get_media_type_string(st->codecpar->codec_type);
    avio_printf(s->pb, "codec_type=%s\n", (const char *)av_x_if_null(type, "unknown"));

    avio_printf(s->pb, "stream_index=%d\n", pkt->stream_index);
    if (pkt->pts != AV_NOPTS_VALUE) {
        avio_printf(s->pb, "pts_time=%f\n", av_q2d(st->time_base) * pkt->pts);
        avio_printf(s->pb, "pts=%"SCNi64"\n", pkt->pts);
    } else {
        avio_printf(s->pb, "pts=N/A\n");
    }
    if (pkt->dts != AV_NOPTS_VALUE) {
        avio_printf(s->pb, "dts_time=%f\n", av_q2d(st->time_base) * pkt->dts);
        avio_printf(s->pb, "dts=%"SCNi64"\n", pkt->dts);
    } else {
        avio_printf(s->pb, "dts=N/A\n");
    }
    if (pkt->duration != 0) {
        avio_printf(s->pb, "duration_time=%f\n", av_q2d(st->time_base) * pkt->duration);
        avio_printf(s->pb, "duration=%"SCNi64"\n", pkt->duration);
    } else {
        avio_printf(s->pb, "duration=N/A\n");
    }

    avio_printf(s->pb, "flags=%c\n", pkt->flags&AV_PKT_FLAG_KEY ? 'K' : '_');

    ffprobe_write_data(s, pkt->data, pkt->size);
    avio_printf(s->pb, "[/PACKET]\n");

    return 0;
}

AVOutputFormat ff_ffprobe_muxer = {
    .name          = "ffprobe",
    .long_name     = NULL_IF_CONFIG_SMALL("FFprobe muxer"),
    .extensions    = "ffprobe",
    .write_header  = ffprobe_write_header,
    .write_packet  = ffprobe_write_packet,
    .audio_codec   = AV_CODEC_ID_MP3,
    .video_codec   = AV_CODEC_ID_MPEG4,
};
