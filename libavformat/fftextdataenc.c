/*
 * Copyright (c) 2016 Stefano Sabatini
 *
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
 * timestamped data virtual muxer
 */

#include "avformat.h"
#include "libavutil/base64.h"

typedef struct {
    uint8_t *buf;
    size_t   buf_size;
} FFTextdataContext;

static int fftextdata_write_header(AVFormatContext *s)
{
    FFTextdataContext *td = s->priv_data;

    td->buf = NULL;
    td->buf_size = 0;

    return 0;
}

static int fftextdata_write_trailer(AVFormatContext *s)
{
    FFTextdataContext *td = s->priv_data;

    av_freep(&td->buf);
    td->buf_size = 0;

    return 0;
}

static int fftextdata_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    FFTextdataContext *td = s->priv_data;
    char ts[32];
    size_t encoded_data_size;
    AVStream *st = s->streams[pkt->stream_index];
    int64_t pts = pkt->pts;
    double secs;
    int hours, mins;

    if (st->start_time != AV_NOPTS_VALUE)
        pts += st->start_time;

    secs = (double)pkt->pts * av_q2d(st->time_base);
    mins  = (int)secs / 60;
    secs  = secs - mins * 60;
    hours = mins / 60;
    mins %= 60;
    snprintf(ts, sizeof(ts), "%d:%02d:%09.6f", hours, mins, secs);
    avio_put_str(s->pb, ts);
    avio_skip(s->pb, -1);
    avio_w8(s->pb, '\n');

    encoded_data_size = AV_BASE64_SIZE(pkt->size);
    if (encoded_data_size > td->buf_size) {
        td->buf = av_realloc_f(td->buf, encoded_data_size, 1);
        if (!td->buf)
            return AVERROR(ENOMEM);
        td->buf_size = encoded_data_size;
    }

    av_base64_encode(td->buf, td->buf_size, pkt->data, pkt->size);
    avio_put_str(s->pb, td->buf);
    avio_skip(s->pb, -1);

    avio_put_str(s->pb, "\n;\n");
    avio_skip(s->pb, -1);

    return 0;
}

AVOutputFormat ff_fftextdata_muxer = {
    .name          = "fftextdata",
    .long_name     = NULL_IF_CONFIG_SMALL("Timestamped data virtual muxer"),
    .extensions    = "fftextdata,fftd",
    .priv_data_size = sizeof(FFTextdataContext),
    .write_header  = fftextdata_write_header,
    .write_packet  = fftextdata_write_packet,
    .write_trailer = fftextdata_write_trailer,
};
