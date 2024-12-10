/*
 * MX Dynamic Adaptive Streaming over Http demuxer
 * Copyright (c) 2020 zheng.lin@mxplayer.in based on HLS demux
 * Copyright (c) 2020 Zheng Lin
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
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavutil/md5.h"
#include "libavutil/parseutils.h"
#include "internal.h"
#include "avio_internal.h"

#ifdef MXD_BUILTIN
static const int MIN_SIZE                  = 524;
static const int NEMO_HEADER_LENGTH_OFFSET = 508;

static const int HEADER_IDENTIFIER_OFFSET  = 512;
static const int HEADER_LENGTH_OFFSET      = 492;
static const int HEADER_MD5_OFFSET         = 496;
#define INITIAL_BUFFER_SIZE 32768
static const char* FILE_IDENTIFIERS[]  = { "NEMO ENCRYPT", "56d3fbd2a209" };

enum MXDChunkType
{
    PREPEND = 0,
    AD,
    VIDEO,
    AUDIO,
    THUMBNAIL,
    TOTAL
};

typedef struct MXDChunk {
    enum MXDChunkType type;
    uint8_t encrypted;
    int64_t start;
    int64_t end;
    int64_t size;
    int64_t nonencrypted_size;
    int64_t encrypted_size;
    int64_t encrypted_offset;
    AVIOContext *input;
    AVFormatContext *ctx;
    AVFormatContext *parent;
    int stream_index_map_size;
    int *stream_index_map;
    AVPacket pkt;
    int64_t cur_timestamp;
    uint8_t eof;
} MXDChunk;

typedef struct MXDContext {
    const AVClass *class;
    AVIOInterruptCB *interrupt_callback;
    int64_t file_size;
    uint32_t header_size;
    uint32_t encrypted_video_size;
    uint32_t ad_size;
    uint32_t video_size;
    uint32_t audio_size;
    uint32_t thumbnail_size;
    uint32_t video_duration;
    uint32_t audio_duration;
    uint32_t video_width;
    uint32_t video_height;
    uint32_t video_degree;
    uint32_t metadata_size;
    const char* metadata;
    MXDChunk chunks[TOTAL];
} MXDContext;

/*
 * It is better to check buffer and size in caller part.
 */
static void decrypt(unsigned char* buf, int size)
{
    /*
     * Data decryption
     */
    for (int i = 0; i < size; ++i) {
        buf[i] ^= 73;
    }
}

static int open_chunk_input(struct MXDChunk *chunk) {
    /*
     * First of all, open required url and seek to start point.
     */
    MXDContext *c = chunk->parent->priv_data;
    int ret = avio_open2(&chunk->input, chunk->parent->url, AVIO_FLAG_READ, c->interrupt_callback, NULL);
    if (ret < 0) {
        av_log(c, AV_LOG_ERROR, "Unable to open chunk input.\n");
        goto exit;
    }

    /*
     * For encrypted chunk, its actual start point is encrypted_offset.
     */
    if (avio_seek(chunk->input, chunk->encrypted ? chunk->encrypted_offset : chunk->start, SEEK_SET) < 0) {
        av_log(c, AV_LOG_ERROR, "Unable to seek to chunk start point.\n");
        ret = AVERROR(EIO);
        goto exit;
    }
    ret = 0;
exit:
    return ret;
}

static void close_chunk_input(struct MXDChunk *chunk) {
    if (chunk->input) {
        av_freep(&chunk->input->buffer);
        avio_context_free(&chunk->input);
    }
}

static int64_t seek_data(void *opaque, int64_t offset, int whence)
{
    struct MXDChunk *chunk = opaque;
    MXDContext *c = chunk->parent->priv_data;
    int64_t ret;
    if (!chunk->input) {
        ret = open_chunk_input(chunk);
        if (ret < 0) {
            av_log(c, AV_LOG_ERROR, "Unable to open chunk input.\n");
            goto exit;
        }
    }
    /*
     * reset eof state
     */
    chunk->eof = 0;

    switch(whence) {
        case SEEK_SET:
            if (chunk->encrypted) {
                if (offset >= 0 && offset < chunk->encrypted_size) {
                    offset += chunk->encrypted_offset;
                } else {
                    offset += chunk->start - chunk->encrypted_size;
                }
            } else {
                offset += chunk->start;
            }
            if (avio_seek(chunk->input, offset, whence) < 0) {
                ret = AVERROR(EIO);
                av_log(c, AV_LOG_ERROR, "Unable to seek data.\n");
                goto exit;
            }
            break;

        case SEEK_CUR:
        case SEEK_END:
            if (avio_seek(chunk->input, offset, whence) < 0) {
                ret = AVERROR(EIO);
                av_log(c, AV_LOG_ERROR, "Unable to seek data relatively.\n");
                goto exit;
            }
            break;

        case AVSEEK_SIZE:
            return chunk->size;
            break;
    }
exit:
    return ret;
}

static int read_data(void *opaque, uint8_t *buf, int buf_size)
{
    struct MXDChunk *chunk = opaque;
    MXDContext *c = chunk->parent->priv_data;
    int ret;
    int64_t pos;
    if (!chunk->input) {
        ret = open_chunk_input(chunk);
        if (ret < 0) {
            av_log(c, AV_LOG_ERROR, "Unable to open chunk input.\n");
            goto exit;
        }
    }

    if (chunk->eof) {
        ret = AVERROR_EOF;
        goto exit;
    }
    /*
     *
     */
    pos = avio_tell(chunk->input);
    if (chunk->encrypted) {
        /*
         * Best effort read logic
         */
        if (pos >= chunk->encrypted_offset) {
            /*
             * If remaining encrypted data could fulfill the request, read
             * and decrypt buffer directly.
             */
            if (pos + buf_size < chunk->end) {
                ret = avio_read(chunk->input, buf, buf_size);
                if (ret < 0) {
                    av_log(c, AV_LOG_ERROR, "Unable to read buffer %s\n", av_err2str(ret));
                    goto exit;
                }
                /*
                 * Data decryption
                 */
                decrypt(buf, ret);
            } else {
                /*
                 * If remaining encrypted data could not fulfill the request, read
                 * all remaining encrypted and decrypt them at first.
                 */
                int64_t encrypted_size = chunk->end - pos;
                ret = avio_read(chunk->input, buf, encrypted_size);
                if (ret < 0) {
                    av_log(c, AV_LOG_ERROR, "Unable to read buffer %s\n", av_err2str(ret));
                    goto exit;
                }
                /*
                 * Data decryption
                 */
                decrypt(buf, ret);

                /*
                 * Seek to the non-encrypted start position and read non-encrypted data.
                 */
                if (avio_seek(chunk->input, chunk->start, SEEK_SET) < 0) {
                    av_log(c, AV_LOG_ERROR, "Unable to reset read position.\n");
                    goto exit;
                }

                int nonecrypted_size = avio_read(chunk->input, buf + encrypted_size, FFMIN(buf_size - encrypted_size, chunk->nonencrypted_size));
                if (nonecrypted_size < 0) {
                    av_log(c, AV_LOG_ERROR, "Unable to read buffer %s\n", av_err2str(nonecrypted_size));
                    goto exit;
                }
                /*
                 * Update the data size by adding non-encrypted data size.
                 */
                ret += nonecrypted_size;
            }
        } else {
            /*
             * We are reading non-encrypted data now.Simply read them directly.
             */
            ret = avio_read(chunk->input, buf, FFMIN(buf_size, (chunk->encrypted_offset - pos)));
            if (ret < 0) {
                av_log(c, AV_LOG_ERROR, "Unable to read buffer %s\n", av_err2str(ret));
                goto exit;
            }

            if (avio_tell(chunk->input) >= chunk->encrypted_offset) {
                chunk->eof = 1;
                goto exit;
            }
        }
    } else {
            ret = avio_read(chunk->input, buf, FFMIN(chunk->end - pos, buf_size));
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Unable to read buffer.\n");
                goto exit;
            }

            if (avio_tell(chunk->input) >= chunk->end) {
                chunk->eof = 1;
            }
    }
exit:
    return ret;
}

static void close_demuxer_for_chunk(struct MXDChunk *chunk)
{
    close_chunk_input(chunk);

    if (chunk->ctx) {
        av_freep(&chunk->ctx->pb->buffer);
        avformat_close_input(&chunk->ctx);
    }

    if (chunk->stream_index_map) {
        av_freep(&chunk->stream_index_map);
    }
}

static int do_open_demuxer_for_chunk(AVFormatContext *s, struct MXDChunk *chunk)
{
    AVIOContext *avio_ctx = NULL;
    uint8_t *avio_ctx_buffer  = NULL;
    size_t avio_ctx_buffer_size = INITIAL_BUFFER_SIZE;
    int ret = 0;

    if (ff_check_interrupt(&s->interrupt_callback)) {
        ret = AVERROR_EXIT;
        av_log(s, AV_LOG_DEBUG, "Exit requested by user.\n");
        goto fail;
    }

    if (!(chunk->ctx = avformat_alloc_context())) {
        ret = AVERROR(ENOMEM);
        av_log(s, AV_LOG_ERROR, "Unable to create AVFormatContext for chunk.\n");
        goto fail;
    }

    avio_ctx_buffer = av_malloc(avio_ctx_buffer_size);
    if (!avio_ctx_buffer) {
        ret = AVERROR(ENOMEM);
        av_log(s, AV_LOG_ERROR, "Unable to allocate buffer for chunk.\n");
        goto fail;
    }

    avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size,
                                  0, chunk, &read_data, NULL, seek_data);
    if (!avio_ctx) {
        ret = AVERROR(ENOMEM);
        av_log(s, AV_LOG_ERROR, "Unable to allocate AVIOContext.\n");
        goto fail;
    }
    chunk->ctx->pb = avio_ctx;

    ret = avformat_open_input(&chunk->ctx, s->url, NULL, NULL);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Unable to open input url %s.\n", s->url);
        goto fail;
    }

    ret = avformat_find_stream_info(chunk->ctx, NULL);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Unable to find stream info.\n");
        goto fail;
    }

#ifdef DEBUG
    av_dump_format(chunk->ctx, 0, s->url, 0);
#endif
    ret = 0;
fail:
    return ret;
}

static int open_demuxer_for_chunk(AVFormatContext *s, struct MXDChunk *chunk, int *stream_index)
{
    int ret = 0;
    int i;

    ret = do_open_demuxer_for_chunk(s, chunk);
    if (ret < 0) {
        goto fail;
    }
    chunk->stream_index_map_size = chunk->ctx->nb_streams;
    chunk->stream_index_map = (int*)av_malloc_array(chunk->stream_index_map_size, sizeof(int));
    if (!chunk->stream_index_map) {
        av_log(s, AV_LOG_ERROR, "Unable to allocate stream index map.");
        goto fail;
    }

    for (i = 0; i < chunk->ctx->nb_streams; i++) {
        AVStream *st = avformat_new_stream(s, NULL);
        AVStream *ist = chunk->ctx->streams[i];
        if (!st) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        st->id = i;
        chunk->stream_index_map[i] = (*stream_index)++;
        // ff_stream_encode_params_copy(st, ist);
        avcodec_parameters_copy(st->codecpar, ist->codecpar);
        if (ist->metadata) {
            av_dict_copy(&st->metadata, ist->metadata, 0);
        }
        avpriv_set_pts_info(st, ist->pts_wrap_bits, ist->time_base.num, ist->time_base.den);
        st->start_time = ist->start_time;
        st->duration = ist->duration;
        st->disposition = ist->disposition;
        st->discard = ist->discard;
    }

    ret = 0;
fail:
    return ret;
}

static int mxd_read_header(AVFormatContext *s)
{
    MXDContext *c = s->priv_data;
    int ret = 0;
    uint8_t *buffer = NULL;
    AVIOContext *pb = s->pb;
    int stream_index = 0;
    /*
     * The structure of vidmate file format is very strange
     * because its file header(file structure description)
     * resides at the end of file.Thus, it requres us to
     * seek to the end at first.
     */
    if (pb->seekable & AVIO_SEEKABLE_NORMAL) {
        c->interrupt_callback = &s->interrupt_callback;

        c->file_size = avio_size(pb);
        if (c->file_size < MIN_SIZE)
        {
            av_log(s, AV_LOG_ERROR, "File size too small.\n");
            ret = AVERROR_INVALIDDATA;
            goto exit;
        }

        ret = avio_seek(pb, c->file_size - MIN_SIZE, SEEK_SET);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Unable to seek to file header.\n");
            goto exit;
        }

        buffer = av_malloc(MIN_SIZE);
        if (!buffer) {
            av_log(s, AV_LOG_ERROR, "Unable to allocate file header buffer.\n");
            ret = AVERROR(ENOMEM);
            goto exit;
        }

        ret = avio_read(pb, buffer, MIN_SIZE);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Unable to read file header buffer.\n");
            ret = AVERROR(EIO);
            goto exit;
        }

        /*
         * It is necessary to check the flavor of vidmate file format
         * because the file header differs from each other.Until now,
         * only two flavors are supproted and all of the are listed
         * as bellows:
         * 0:NEMO ENCRYPT
         * |File Description(File Header Length - 4B)|File Header Length(4B)|File Identifier(12B)
         * 1:56d3fbd2a209
         * |File Description(File Header Length - 4B:E)|File Header Length(4B:E)|File Checksum(16B)|File Identifier(12B)
         */
        ret = -1;
        for (int i = 0; i < FF_ARRAY_ELEMS(FILE_IDENTIFIERS); ++i) {
            if (0 == memcmp(buffer + HEADER_IDENTIFIER_OFFSET, FILE_IDENTIFIERS[i], strlen(FILE_IDENTIFIERS[i]) - 1)) {
                ret = i;
                break;
            }
        }

        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Unsupported file format.\n");
            ret = AVERROR(EINVAL);
            goto exit;
        }

        if (ret == 0) {
            /*
             * "NEMO ENCRYPT"
             */
            c->header_size = AV_RB32(buffer + 508);
            if (c->header_size < 508 ) {
                uint8_t *header = buffer + 512 - c->header_size;
                c->encrypted_video_size = AV_RB32(header);
                c->ad_size = AV_RB32(header + 4);
                c->video_size = AV_RB32(header + 8);
                c->audio_size = AV_RB32(header + 12);
                c->thumbnail_size = AV_RB32(header + 16);
                c->video_duration = AV_RB32(header + 20);
                c->audio_duration = AV_RB32(header + 24);
                c->video_width = AV_RB32(header + 28);
                c->video_height = AV_RB32(header + 32);
                c->video_degree = AV_RB32(header + 36);
                c->metadata_size = AV_RB32(header + 40);
                c->metadata = av_strndup(header + 44, c->metadata_size);
            }
        } else if (ret == 1) {
            /*
             * "56d3fbd2a209"
             */

            /*
             * Header length field is encrypted, so it is necessary
             * to copy it into another buffer and decrypt.
             */
            uint8_t length[4];
            memcpy(length, buffer + HEADER_LENGTH_OFFSET, 4);
            decrypt(length, sizeof(length));
            c->header_size = AV_RB32(length);

            /*
             * Find and adjust the offset of vidmate file header.
             */
            int offset = HEADER_LENGTH_OFFSET - (c->header_size - 4);
            if (offset < 0) {
                int size = c->header_size + 16;
                ret = av_reallocp(&buffer, size);
                if (ret < 0) {
                    av_log(s, AV_LOG_ERROR, "Unable to allocate buffer for header.\n");
                    ret = AVERROR(ENOMEM);
                    goto exit;
                }

                ret = avio_seek(pb, c->file_size - size - 12, SEEK_SET);
                if (ret < 0) {
                    av_log(s, AV_LOG_ERROR, "Unable to seek to probe point.\n");
                    goto exit;
                }

                ret = avio_read(pb, buffer, size);
                if (ret < 0) {
                    av_log(s, AV_LOG_ERROR, "Unable to read buffer.\n");
                    ret = AVERROR(EIO);
                    goto exit;
                }
                offset = 0;
            }

            uint8_t *header = buffer + offset;
            uint8_t checksum[16];
            av_md5_sum(checksum, header, c->header_size);

            uint8_t *header_md5 = buffer + HEADER_MD5_OFFSET;
            if (0 != memcmp(checksum, header_md5, 16)) {
                av_log(s, AV_LOG_ERROR, "Failed to verify md5.\n");
                ret = AVERROR(EINVAL);
                goto exit;
            }

            /*
             * The file header is encrypted, so decrypt it at first.
             */
            decrypt(header, c->header_size);
            int encrypt_version = AV_RB32(header);

            if (encrypt_version <=0) {
                av_log(NULL, AV_LOG_ERROR, "Invalid version.");
                ret = AVERROR_INVALIDDATA;
                goto exit;
            }
            if (encrypt_version <=2) {
                // buffer = header;
            }
            // c->encrypt_version = -2;

            if (encrypt_version == -1 || encrypt_version == -1) {

            } else if (encrypt_version == 2) {
                uint32_t encrypt_version = AV_RB32(header);
                c->encrypted_video_size = AV_RB32(header + 4);
                c->video_size = AV_RB64(header + 8);
                c->audio_size = AV_RB32(header + 16);
                c->thumbnail_size = AV_RB32(header + 20);
                c->video_duration = AV_RB32(header + 24);
                c->audio_duration = AV_RB32(header + 28);
                c->video_width = AV_RB32(header + 32);
                c->video_height = AV_RB32(header + 36);
                c->video_degree = AV_RB32(header + 40);
                // skip 32 bytes
                c->metadata_size = AV_RB32(header + 72);
                if (c->metadata_size > 0) {

                }
            }


        }

        // c->header_size = AV_RB32(buffer);
        // ret = av_reallocp(&buffer, c->header_size + PROBE_BUF_SIZE - 4);
        // if (ret < 0) {
        //     av_log(s, AV_LOG_ERROR, "Unable to allocate buffer for header.\n");
        //     ret = AVERROR(ENOMEM);
        //     goto exit;
        // }

        // ret = avio_seek(pb, c->file_size - c->header_size - (PROBE_BUF_SIZE - 4), SEEK_SET );
        // if (ret < 0) {
        //     av_log(s, AV_LOG_ERROR, "Unable to seek to header.\n");
        //     goto exit;
        // }

        // ret = avio_read(pb, buffer, c->header_size + ( PROBE_BUF_SIZE - 4 ));
        // if (ret < 0) {
        //     av_log(s, AV_LOG_ERROR, "Unable to read file header.\n");
        //     ret = AVERROR(EIO);
        //     goto exit;
        // }

#ifdef DEBUG
        ff_dlog(s, "File info:\n" );
        ff_dlog(s, "\tfile size            : %lld\n", c->file_size);
        ff_dlog(s, "\tencrypted video size : %u\n", c->encrypted_video_size);
        ff_dlog(s, "\tad size              : %u\n", c->ad_size);
        ff_dlog(s, "\tvideo size           : %u\n", c->video_size);
        ff_dlog(s, "\taudio size           : %u\n", c->audio_size);
        ff_dlog(s, "\tthumb size           : %u\n", c->thumbnail_size);
        ff_dlog(s, "\tvideo duration       : %u\n", c->video_duration);
        ff_dlog(s, "\taudio duration       : %u\n", c->audio_duration);
        ff_dlog(s, "\tvideo width          : %u\n", c->video_width);
        ff_dlog(s, "\tvideo height         : %u\n", c->video_height);
        ff_dlog(s, "\tvideo degree         : %u\n", c->video_degree);
        ff_dlog(s, "\tmeta data size       : %u\n", c->metadata_size);
        ff_dlog(s, "\tmeta data            : %s\n", c->metadata);
#endif
        /*
         * Accroding to the test result, encrypted_video_size equals to the
         * size of prepended video.
         */
        c->chunks[PREPEND].type = PREPEND;
        c->chunks[PREPEND].encrypted = 0;
        c->chunks[PREPEND].start = 0;
        c->chunks[PREPEND].end = c->encrypted_video_size;
        c->chunks[PREPEND].size = c->encrypted_video_size;
        c->chunks[PREPEND].nonencrypted_size = c->chunks[PREPEND].size;
        c->chunks[PREPEND].encrypted_size = 0;
        c->chunks[PREPEND].encrypted_offset = c->chunks[PREPEND].end;
        c->chunks[PREPEND].parent = s;

        /*
         * We didn't see any file contains ad.
         */
        c->chunks[AD].type = AD;
        c->chunks[AD].encrypted = 0;
        c->chunks[AD].start = c->chunks[PREPEND].end;
        c->chunks[AD].end = c->chunks[AD].start + c->ad_size;
        c->chunks[AD].size = c->ad_size;
        c->chunks[AD].nonencrypted_size = c->chunks[AD].size;
        c->chunks[AD].encrypted_size = 0;
        c->chunks[AD].encrypted_offset = c->chunks[AD].end;
        c->chunks[AD].parent = s;

        /*
         * Until now, it seems that only video chunk is encrypted.
         */
        c->chunks[VIDEO].type = VIDEO;
        c->chunks[VIDEO].encrypted = 1;
        c->chunks[VIDEO].start = c->chunks[AD].end;
        c->chunks[VIDEO].end = c->chunks[VIDEO].start + c->video_size;
        c->chunks[VIDEO].size = c->video_size;
        c->chunks[VIDEO].nonencrypted_size = c->video_size - c->encrypted_video_size;
        c->chunks[VIDEO].encrypted_size = c->encrypted_video_size;
        c->chunks[VIDEO].encrypted_offset = c->chunks[VIDEO].start + c->chunks[VIDEO].nonencrypted_size;
        c->chunks[VIDEO].parent = s;

        /*
         * Audio chunk is not encrypted.
         */
        c->chunks[AUDIO].type = AUDIO;
        c->chunks[AUDIO].encrypted = 0;
        c->chunks[AUDIO].start = c->chunks[VIDEO].end;
        c->chunks[AUDIO].end = c->chunks[AUDIO].start + c->audio_size;
        c->chunks[AUDIO].size = c->audio_size;
        c->chunks[AUDIO].nonencrypted_size = c->chunks[AUDIO].size;
        c->chunks[AUDIO].encrypted_size = 0;
        c->chunks[AUDIO].encrypted_offset = c->chunks[AUDIO].end;
        c->chunks[AUDIO].parent = s;

        /*
         * This may boost up thumbnail generation.
         */
        c->chunks[THUMBNAIL].type = THUMBNAIL;
        c->chunks[THUMBNAIL].encrypted = 0;
        c->chunks[THUMBNAIL].start = c->chunks[AUDIO].end;
        c->chunks[THUMBNAIL].end = c->chunks[THUMBNAIL].start + c->thumbnail_size;
        c->chunks[THUMBNAIL].size = c->thumbnail_size;
        c->chunks[THUMBNAIL].nonencrypted_size = c->chunks[THUMBNAIL].size;
        c->chunks[THUMBNAIL].encrypted_size = 0;
        c->chunks[THUMBNAIL].encrypted_offset = c->chunks[THUMBNAIL].end;
        c->chunks[THUMBNAIL].parent = s;

        /*
         * Currently, only VIDEO and AUDIO chunk would be rendereded in playback.
         */
        for (int i = VIDEO; i < THUMBNAIL; ++i) {
            if (c->chunks[i].size > 0) {
                ret = open_demuxer_for_chunk(s, &c->chunks[i], &stream_index);
                if (ret < 0) {
                    av_log(c, AV_LOG_ERROR, "Unabel to open demuxer for chunk.\n");
                    goto exit;
                }
            }
        }

        /*
         * Copy metadata.Video has higher priority than Audio.
         */
        AVDictionary *metadata = NULL;
        if (c->chunks[VIDEO].ctx) {
            metadata = c->chunks[VIDEO].ctx->metadata;
        } else if (c->chunks[AUDIO].ctx) {
            metadata = c->chunks[AUDIO].ctx->metadata;
        }
        if (metadata) {
            av_dict_copy(&s->metadata, metadata, 0);
        }

    } else {
        av_log(s, AV_LOG_ERROR, "File is not seekable.\n");
        ret = AVERROR(EIO);;
        goto exit;
    }
    ret = 0;
exit:
    if (buffer) {
        av_freep(&buffer);
    }
    return ret;
}

static int mxd_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MXDContext *c = s->priv_data;
    int ret = 0;
    int64_t mints = 0;

    MXDChunk *cur = NULL;
    MXDChunk *video = &c->chunks[VIDEO];
    if (video->ctx) {
        if (video->pkt.pts != AV_NOPTS_VALUE) {
            av_packet_ref(pkt, &video->pkt);
            if (pkt->stream_index >= 0 && pkt->stream_index < video->stream_index_map_size) {
                pkt->stream_index = video->stream_index_map[pkt->stream_index];
            }
            av_packet_unref(&video->pkt);
            return 0;
        }
        if (!cur || video->cur_timestamp < mints) {
            cur = video;
            mints = video->cur_timestamp;
        }
    }

    MXDChunk *audio = &c->chunks[AUDIO];
    if (audio->ctx) {
        if (audio->pkt.pts != AV_NOPTS_VALUE) {
            av_packet_ref(pkt, &audio->pkt);
            if (pkt->stream_index >= 0 && pkt->stream_index < video->stream_index_map_size) {
                pkt->stream_index = audio->stream_index_map[pkt->stream_index];
            }
            av_packet_unref(&audio->pkt);
            return 0;
        }
        if (!cur || audio->cur_timestamp < mints) {
            cur = audio;
            mints = audio->cur_timestamp;
        }
    }

    if (!cur) {
        return AVERROR_INVALIDDATA;
    }

    while (!ff_check_interrupt(c->interrupt_callback) && !ret) {
        ret = av_read_frame(cur->ctx, pkt);
        if (ret >= 0) {
            /*
             * 1.Update current timestamp which is used to determine the most appropriate
             * packet to be returned.
             * 2.Update the local stream index to global stream index.
             */
            cur->cur_timestamp = av_rescale_q(pkt->pts, cur->ctx->streams[pkt->stream_index]->time_base, AV_TIME_BASE_Q);
            if (pkt->stream_index >= 0 && pkt->stream_index < cur->stream_index_map_size) {
                pkt->stream_index = cur->stream_index_map[pkt->stream_index];
            }
            return 0;
        }
    }
    return AVERROR_EOF;
}

static int mxd_read_close(AVFormatContext *s)
{
    MXDContext *c = s->priv_data;
    for (int i = PREPEND; i < TOTAL; ++i) {
        close_demuxer_for_chunk(&c->chunks[i]);
    }
    if (c->metadata) {
        av_freep(&c->metadata);
    }
    return 0;
}

static int mxd_read_seek(AVFormatContext *s, int stream_index, int64_t timestamp, int flags)
{
    MXDContext *c = s->priv_data;
    timestamp = av_rescale_q(timestamp, s->streams[stream_index]->time_base, AV_TIME_BASE_Q);
    MXDChunk *video = &c->chunks[VIDEO];
    int ret = 0;
    if (video->ctx) {
        ret = av_seek_frame(video->ctx, -1, timestamp, flags);
        if (ret < 0) {
            goto exit;
        }
        video->cur_timestamp = 0;

        av_packet_unref(&video->pkt);
        ret = av_read_frame(video->ctx, &video->pkt);
        if (ret < 0) {
            goto exit;
        }

        /*
         * Adjust timestamp according to the seek result of video.
         */
        if (video->pkt.pts != AV_NOPTS_VALUE) {
            timestamp = av_rescale_q(video->pkt.pts, video->ctx->streams[0]->time_base, AV_TIME_BASE_Q);
        }
    }

    MXDChunk *audio = &c->chunks[AUDIO];
    if (audio->ctx) {
        ret = av_seek_frame(audio->ctx, -1, timestamp, flags);
        if (ret < 0) {
            goto exit;
        }
        audio->cur_timestamp = 0;

        // ret = av_read_frame(audio->ctx, &audio->pkt);
        // if (ret < 0) {
        //     goto exit;
        // }
    }
    ret = 0;
exit:
    return ret;
}

static int mxd_read_probe(const AVProbeData *p)
{
    int score = 0;
    for (int i = 0; i < FF_ARRAY_ELEMS(FILE_IDENTIFIERS); ++i) {
        int length = strlen(FILE_IDENTIFIERS[i]);
        if (p->buf_size >= length) {
            if (0 == memcmp(p->buf + p->buf_size - length, FILE_IDENTIFIERS[i], length)) {
                score = AVPROBE_SCORE_MAX;
                break;
            }
        }
    }
    return score;
}

#define OFFSET(x) offsetof(MXDASHContext, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM
static const AVOption mxd_options[] = {
    {NULL}
};

static const AVClass mxd_class = {
    .class_name = "mxd",
    .item_name  = av_default_item_name,
    .option     = mxd_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_mxd_demuxer = {
    .name           = "mxd",
    .long_name      = NULL_IF_CONFIG_SMALL("VMD (VM DASH Format)"),
    .flags          = AVFMT_SEEK_TO_PTS,
    .priv_class     = &mxd_class,
    .priv_data_size = sizeof(MXDContext),
    .read_probe     = mxd_read_probe,
    .read_header    = mxd_read_header,
    .read_packet    = mxd_read_packet,
    .read_close     = mxd_read_close,
    .read_seek      = mxd_read_seek,
};

#else
#include "mxd_wrap.h"
static int mxd_wrapper_read_header(AVFormatContext *s)
{
   return mxd_read_header(s);
}

static int mxd_wrapper_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    return mxd_read_packet(s, pkt);
}

static int mxd_wrapper_read_close(AVFormatContext *s)
{
    return mxd_read_close(s);
}

static int mxd_wrapper_read_seek(AVFormatContext *s, int stream_index, int64_t timestamp, int flags)
{
    return mxd_read_seek(s, stream_index, timestamp, flags);
}

static int mxd_wrapper_read_probe(const AVProbeData *p)
{
    return mxd_read_probe(p);
}

#define OFFSET(x) offsetof(MXDASHContext, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM
static const AVOption mxd_options[] = {
    {NULL}
};

static const AVClass mxd_class = {
    .class_name = "mxd",
    .item_name  = av_default_item_name,
    .option     = mxd_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_mxd_demuxer = {
    .name           = "mxd",
    .long_name      = NULL_IF_CONFIG_SMALL("VMD (VM DASH Format)"),
    .flags          = AVFMT_SEEK_TO_PTS,
    .priv_class     = &mxd_class,
    .priv_data_size = 10240,
    .read_probe     = mxd_wrapper_read_probe,
    .read_header    = mxd_wrapper_read_header,
    .read_packet    = mxd_wrapper_read_packet,
    .read_close     = mxd_wrapper_read_close,
    .read_seek      = mxd_wrapper_read_seek,
};

#endif
