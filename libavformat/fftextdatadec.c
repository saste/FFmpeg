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
 * timestamped data virtual demuxer
 */

#include "libavutil/base64.h"
#include "libavutil/bprint.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "avformat.h"
#include "internal.h"

typedef struct {
    const AVClass *class;  /**< Class for private options. */
    int nb_packets;
    AVBPrint bp;
    const char *codec_name;
} FFTextdataContext;

av_cold static int fftextdata_read_close(AVFormatContext *avctx)
{
    FFTextdataContext *td = avctx->priv_data;

    av_bprint_finalize(&td->bp, NULL);
    return 0;
}

av_cold static int fftextdata_read_header(AVFormatContext *s)
{
    FFTextdataContext *td = s->priv_data;
    AVStream *st;
    const AVCodecDescriptor *cd;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    cd = avcodec_descriptor_get_by_name(td->codec_name);
    if (!cd) {
        av_log(s, AV_LOG_ERROR, "Impossible to find a codec with name '%s'\n",
               td->codec_name);
        return AVERROR(EINVAL);
    }

    st->codecpar->codec_type = cd->type;
    st->codecpar->codec_id = cd->id;
    avpriv_set_pts_info(st, 64, 1, 1000000);

    av_bprint_init(&(td->bp), 0, 1);
    td->nb_packets = 0;

    return 0;
}

static inline int is_space(char c)
{
    return c == ' '  || c == '\t' || c == '\r' || c == '\n';
}

static int read_word(AVIOContext *avio, AVBPrint *bp)
{
    int c;

    av_bprint_clear(bp);

    /* skip spaces */
    do {
        c = avio_r8(avio);
        if (!c)
            goto end;
    } while (is_space(c));

    /* read word */
    av_bprint_chars(bp, c, 1);
    do {
        c = avio_r8(avio);
        if (!c)
            goto end;
        if (is_space(c)) {
            avio_skip(avio, -1);
            goto end;
        }
        av_bprint_chars(bp, c, 1);
    } while (1);

end:
    return bp->len;
}

static int read_data(AVIOContext *avio, AVBPrint *bp)
{
    int c;

    av_bprint_clear(bp);

    /* skip spaces */
    do {
        c = avio_r8(avio);
        if (!c)
            goto end;
    } while (is_space(c));

    /* read data chunk */
    av_bprint_chars(bp, c, 1);
    do {
        c = avio_r8(avio);
        if (!c || c == ';')
            goto end;
        if (is_space(c)) {
            continue;
        }
        av_bprint_chars(bp, c, 1);
    } while (1);

end:
    return bp->len;
}

static int fftextdata_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    FFTextdataContext *td = s->priv_data;
    AVIOContext *avio = s->pb;
    int ret;
    AVBPrint *bp = &(td->bp);

    pkt->pos = avio_tell(avio);

    /* read PTS  */
    ret = read_word(avio, bp);
    if (ret == 0)
        return AVERROR_EOF;

    ret = av_parse_time(&pkt->pts, bp->str, 1);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Invalid time specification '%s' for data packet #%d\n",
               bp->str, td->nb_packets);
        return ret;
    }

    ret = read_data(avio, bp);
    if (ret == 0) {
        av_log(s, AV_LOG_WARNING, "Incomplete packet #%d with no data at the end of the data stream\n",
               td->nb_packets);
        return AVERROR_EOF;
    }

    pkt->size = AV_BASE64_DECODE_SIZE(ret);
    pkt->data = av_malloc(pkt->size);
    if (ret < 0)
        return ret;

    ret = av_base64_decode(pkt->data, bp->str, pkt->size);
    if (ret < 0) {
        av_freep(&pkt->data);
        return ret;
    }

    pkt->size = ret;
    pkt->flags |= AV_PKT_FLAG_KEY;
    td->nb_packets++;

    return ret;
}

#define OFFSET(x) offsetof(FFTextdataContext, x)

#define D AV_OPT_FLAG_DECODING_PARAM

#define OFFSET(x) offsetof(FFTextdataContext, x)

static const AVOption options[] = {
    { "codec_name",  "set output codec name", OFFSET(codec_name), AV_OPT_TYPE_STRING, {.str = "bin_data"}, CHAR_MIN, CHAR_MAX, D },
    { NULL },
};

static const AVClass fftextdata_class = {
    .class_name = "fftexdata demuxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_fftextdata_demuxer = {
    .name           = "fftextdata",
    .long_name      = NULL_IF_CONFIG_SMALL("Timestamped data virtual demuxer"),
    .extensions     = "fftextdata,fftd",
    .priv_data_size = sizeof(FFTextdataContext),
    .read_header    = fftextdata_read_header,
    .read_packet    = fftextdata_read_packet,
    .read_close     = fftextdata_read_close,
    .priv_class     = &fftextdata_class,
};
