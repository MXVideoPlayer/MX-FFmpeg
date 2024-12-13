//
//  mxvencoder.c
/*
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

#include <stdint.h>

#include "av1.h"
#include "avc.h"
#include "hevc.h"
#include "avformat.h"
#include "avio_internal.h"
#include "avlanguage.h"
#include "flacenc.h"
#include "internal.h"
#include "isom.h"
#include "mxv.h"
#include "riff.h"
#include "subtitles.h"
#include "vorbiscomment.h"
#include "wv.h"

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/crc.h"
#include "libavutil/dict.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/lfg.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/random_seed.h"
#include "libavutil/rational.h"
#include "libavutil/samplefmt.h"
#include "libavutil/sha.h"
#include "libavutil/stereo3d.h"
#include "libavutil/base64.h"

#include "libavcodec/xiph.h"
#include "libavcodec/mpeg4audio.h"
#include "libavcodec/internal.h"

#if !CONFIG_MXV_FROM_MXVP
typedef struct ebml_master {
    int64_t         pos;                ///< absolute offset in the containing AVIOContext where the master's elements start
    int             sizebytes;          ///< how many bytes were reserved for the size
} ebml_master;

typedef struct mxv_seekhead_entry {
    uint32_t        elementid;
    uint64_t        segmentpos;
} mxv_seekhead_entry;

typedef struct mxv_seekhead {
    int64_t                 filepos;
    int64_t                 segment_offset;     ///< the file offset to the beginning of the segment
    int                     reserved_size;      ///< -1 if appending to file
    int                     max_entries;
    mxv_seekhead_entry      *entries;
    int                     num_entries;
} mxv_seekhead;

typedef struct mxv_cuepoint {
    uint64_t        pts;
    int             stream_idx;
    int             tracknum;
    int64_t         cluster_pos;        ///< file offset of the cluster containing the block
    int64_t         relative_pos;       ///< relative offset from the position of the cluster containing the block
    int64_t         duration;           ///< duration of the block according to time base
} mxv_cuepoint;

typedef struct mxv_cues {
    int64_t         segment_offset;
    mxv_cuepoint    *entries;
    int             num_entries;
} mxv_cues;

typedef struct mxv_track {
    int             write_dts;
    int             has_cue;
    int             sample_rate;
    int64_t         sample_rate_offset;
    int64_t         codecpriv_offset;
    int64_t         ts_offset;
} mxv_track;

typedef struct mxv_attachment {
    int             stream_idx;
    uint32_t        fileuid;
} mxv_attachment;

typedef struct mxv_attachments {
    mxv_attachment  *entries;
    int             num_entries;
} mxv_attachments;

#define MODE_MXVv2 0x01
#define MODE_WEBM       0x02

/** Maximum number of tracks allowed in a MXV file (with track numbers in
 * range 1 to 126 (inclusive) */
#define MAX_TRACKS 126

typedef struct MXVMuxContext {
    const AVClass   *class;
    int             mode;
    AVIOContext     *tags_bc;
    int64_t         tags_pos;
    AVIOContext     *info_bc;
    int64_t         info_pos;
    AVIOContext     *tracks_bc;
    int64_t         tracks_pos;
    ebml_master     segment;
    int64_t         segment_offset;
    AVIOContext     *cluster_bc;
    int64_t         cluster_pos;        ///< file offset of the current cluster
    int64_t         cluster_pts;
    int64_t         duration_offset;
    int64_t         duration;
    mxv_seekhead    *seekhead;
    mxv_cues        *cues;
    mxv_track       *tracks;
    mxv_attachments *attachments;

    AVPacket        cur_audio_pkt;

    int have_attachments;
    int have_video;

    int reserve_cues_space;
    int cluster_size_limit;
    int64_t cues_pos;
    int64_t cluster_time_limit;
    int is_dash;
    int dash_track_number;
    int is_live;
    int write_crc;

    const uint8_t *aes_key;

    uint32_t chapter_id_offset;
    int wrote_chapters;

    int64_t last_track_timestamp[MAX_TRACKS];

    int64_t *stream_durations;
    int64_t *stream_duration_offsets;

    int allow_raw_vfw;
} MXVMuxContext;

/** 2 bytes * 7 for EBML IDs, 7 1-byte EBML lengths, 6 1-byte uint,
 * 8 byte for "matroska" doctype string */
#define MAX_EBML_HEADER_SIZE 35

/** 2 bytes * 3 for EBML IDs, 3 1-byte EBML lengths, 8 bytes for 64 bit
 * offset, 4 bytes for target EBML ID */
#define MAX_SEEKENTRY_SIZE 21

/** per-cuepoint-track - 5 1-byte EBML IDs, 5 1-byte EBML sizes, 3 8-byte uint max
 * and one 1-byte uint for the track number (this assumes MAX_TRACKS to be <= 255) */
#define MAX_CUETRACKPOS_SIZE 35

/** per-cuepoint - 1 1-byte EBML ID, 1 1-byte EBML size, 8-byte uint max */
#define MAX_CUEPOINT_CONTENT_SIZE(num_tracks) 10 + MAX_CUETRACKPOS_SIZE * num_tracks

/** Seek preroll value for opus */
#define OPUS_SEEK_PREROLL 80000000

static int ebml_id_size(uint32_t id)
{
    return (av_log2(id + 1) - 1) / 7 + 1;
}

static void put_ebml_id(AVIOContext *pb, uint32_t id)
{
    int i = ebml_id_size(id);
    while (i--)
        avio_w8(pb, (uint8_t)(id >> (i * 8)));
}

/**
 * Write an EBML size meaning "unknown size".
 *
 * @param bytes The number of bytes the size should occupy (maximum: 8).
 */
static void put_ebml_size_unknown(AVIOContext *pb, int bytes)
{
    av_assert0(bytes <= 8);
    avio_w8(pb, 0x1ff >> bytes);
    ffio_fill(pb, 0xff, bytes - 1);
}

/**
 * Calculate how many bytes are needed to represent a given number in EBML.
 */
static int ebml_num_size(uint64_t num)
{
    int bytes = 1;
    while ((num + 1) >> bytes * 7)
        bytes++;
    return bytes;
}

/**
 * Write a number in EBML variable length format.
 *
 * @param bytes The number of bytes that need to be used to write the number.
 *              If zero, any number of bytes can be used.
 */
static void put_ebml_num(AVIOContext *pb, uint64_t num, int bytes)
{
    int i, needed_bytes = ebml_num_size(num);

    // sizes larger than this are currently undefined in EBML
    av_assert0(num < (1ULL << 56) - 1);

    if (bytes == 0)
        // don't care how many bytes are used, so use the min
        bytes = needed_bytes;
    // the bytes needed to write the given size would exceed the bytes
    // that we need to use, so write unknown size. This shouldn't happen.
    av_assert0(bytes >= needed_bytes);

    num |= 1ULL << bytes * 7;
    for (i = bytes - 1; i >= 0; i--)
        avio_w8(pb, (uint8_t)(num >> i * 8));
}

static void put_ebml_uint(AVIOContext *pb, uint32_t elementid, uint64_t val)
{
    int i, bytes = 1;
    uint64_t tmp = val;
    while (tmp >>= 8)
        bytes++;

    put_ebml_id(pb, elementid);
    put_ebml_num(pb, bytes, 0);
    for (i = bytes - 1; i >= 0; i--)
        avio_w8(pb, (uint8_t)(val >> i * 8));
}

static void put_ebml_sint(AVIOContext *pb, uint32_t elementid, int64_t val)
{
    int i, bytes = 1;
    uint64_t tmp = 2*(val < 0 ? val^-1 : val);

    while (tmp>>=8) bytes++;

    put_ebml_id(pb, elementid);
    put_ebml_num(pb, bytes, 0);
    for (i = bytes - 1; i >= 0; i--)
        avio_w8(pb, (uint8_t)(val >> i * 8));
}

static void put_ebml_float(AVIOContext *pb, uint32_t elementid, double val)
{
    put_ebml_id(pb, elementid);
    put_ebml_num(pb, 8, 0);
    avio_wb64(pb, av_double2int(val));
}

static void put_ebml_binary(AVIOContext *pb, uint32_t elementid,
                            const void *buf, int size)
{
    put_ebml_id(pb, elementid);
    put_ebml_num(pb, size, 0);
    avio_write(pb, buf, size);
}

static void put_ebml_string(AVIOContext *pb, uint32_t elementid,
                            const char *str)
{
    put_ebml_binary(pb, elementid, str, strlen(str));
}

/**
 * Write a void element of a given size. Useful for reserving space in
 * the file to be written to later.
 *
 * @param size The number of bytes to reserve, which must be at least 2.
 */
static void put_ebml_void(AVIOContext *pb, uint64_t size)
{
    int64_t currentpos = avio_tell(pb);

    av_assert0(size >= 2);

    put_ebml_id(pb, EBML_ID_VOID);
    // we need to subtract the length needed to store the size from the
    // size we need to reserve so 2 cases, we use 8 bytes to store the
    // size if possible, 1 byte otherwise
    if (size < 10)
        put_ebml_num(pb, size - 2, 0);
    else
        put_ebml_num(pb, size - 9, 8);
    ffio_fill(pb, 0, currentpos + size - avio_tell(pb));
}

static ebml_master start_ebml_master(AVIOContext *pb, uint32_t elementid,
                                     uint64_t expectedsize)
{
    int bytes = expectedsize ? ebml_num_size(expectedsize) : 8;

    put_ebml_id(pb, elementid);
    put_ebml_size_unknown(pb, bytes);
    return (ebml_master) { avio_tell(pb), bytes };
}

static void end_ebml_master(AVIOContext *pb, ebml_master master)
{
    int64_t pos = avio_tell(pb);

    if (avio_seek(pb, master.pos - master.sizebytes, SEEK_SET) < 0)
        return;
    put_ebml_num(pb, pos - master.pos, master.sizebytes);
    avio_seek(pb, pos, SEEK_SET);
}

static int start_ebml_master_crc32(AVIOContext *pb, AVIOContext **dyn_cp, MXVMuxContext *mxv,
                                   uint32_t elementid)
{
    int ret;

    if ((ret = avio_open_dyn_buf(dyn_cp)) < 0)
        return ret;

    put_ebml_id(pb, elementid);
    if (mxv->write_crc)
        put_ebml_void(*dyn_cp, 6); /* Reserve space for CRC32 so position/size calculations using avio_tell() take it into account */

    return 0;
}

static void end_ebml_master_crc32(AVIOContext *pb, AVIOContext **dyn_cp, MXVMuxContext *mxv)
{
    uint8_t *buf, crc[4];
    int size, skip = 0;

    size = avio_close_dyn_buf(*dyn_cp, &buf);
    put_ebml_num(pb, size, 0);
    if (mxv->write_crc) {
        skip = 6; /* Skip reserved 6-byte long void element from the dynamic buffer. */
        AV_WL32(crc, av_crc(av_crc_get_table(AV_CRC_32_IEEE_LE), UINT32_MAX, buf + skip, size - skip) ^ UINT32_MAX);
        put_ebml_binary(pb, EBML_ID_CRC32, crc, sizeof(crc));
    }
    avio_write(pb, buf + skip, size - skip);

    av_free(buf);
    *dyn_cp = NULL;
}

/**
* Complete ebml master without destroying the buffer, allowing for later updates
*/
static void end_ebml_master_crc32_preliminary(AVIOContext *pb, AVIOContext **dyn_cp, MXVMuxContext *mxv,
                                              int64_t *pos)
{
    uint8_t *buf;
    int size = avio_get_dyn_buf(*dyn_cp, &buf);

    *pos = avio_tell(pb);

    put_ebml_num(pb, size, 0);
    avio_write(pb, buf, size);
}

static void put_xiph_size(AVIOContext *pb, int size)
{
    ffio_fill(pb, 255, size / 255);
    avio_w8(pb, size % 255);
}

/**
 * Free the members allocated in the mux context.
 */
static void mxv_free(MXVMuxContext *mxv) {
    uint8_t* buf;
    if (mxv->cluster_bc) {
        avio_close_dyn_buf(mxv->cluster_bc, &buf);
        av_free(buf);
    }
    if (mxv->info_bc) {
        avio_close_dyn_buf(mxv->info_bc, &buf);
        av_free(buf);
    }
    if (mxv->tracks_bc) {
        avio_close_dyn_buf(mxv->tracks_bc, &buf);
        av_free(buf);
    }
    if (mxv->tags_bc) {
        avio_close_dyn_buf(mxv->tags_bc, &buf);
        av_free(buf);
    }
    if (mxv->seekhead) {
        av_freep(&mxv->seekhead->entries);
        av_freep(&mxv->seekhead);
    }
    if (mxv->cues) {
        av_freep(&mxv->cues->entries);
        av_freep(&mxv->cues);
    }
    if (mxv->attachments) {
        av_freep(&mxv->attachments->entries);
        av_freep(&mxv->attachments);
    }
    av_freep(&mxv->aes_key);
    av_freep(&mxv->tracks);
    av_freep(&mxv->stream_durations);
    av_freep(&mxv->stream_duration_offsets);
}

/**
 * Initialize a mxv_seekhead element to be ready to index level 1 MXV
 * elements. If a maximum number of elements is specified, enough space
 * will be reserved at the current file location to write a seek head of
 * that size.
 *
 * @param segment_offset The absolute offset to the position in the file
 *                       where the segment begins.
 * @param numelements The maximum number of elements that will be indexed
 *                    by this seek head, 0 if unlimited.
 */
static mxv_seekhead *mxv_start_seekhead(AVIOContext *pb, int64_t segment_offset,
                                        int numelements)
{
    mxv_seekhead *new_seekhead = av_mallocz(sizeof(mxv_seekhead));
    if (!new_seekhead)
        return NULL;

    new_seekhead->segment_offset = segment_offset;

    if (numelements > 0) {
        new_seekhead->filepos = avio_tell(pb);
        // 21 bytes max for a seek entry, 10 bytes max for the SeekHead ID
        // and size, 6 bytes for a CRC32 element, and 3 bytes to guarantee
        // that an EBML void element will fit afterwards
        new_seekhead->reserved_size = numelements * MAX_SEEKENTRY_SIZE + 19;
        new_seekhead->max_entries   = numelements;
        put_ebml_void(pb, new_seekhead->reserved_size);
    }
    return new_seekhead;
}

static int mxv_add_seekhead_entry(mxv_seekhead *seekhead, uint32_t elementid, uint64_t filepos)
{
    mxv_seekhead_entry *entries = seekhead->entries;

    // don't store more elements than we reserved space for
    if (seekhead->max_entries > 0 && seekhead->max_entries <= seekhead->num_entries)
        return -1;

    entries = av_realloc_array(entries, seekhead->num_entries + 1, sizeof(mxv_seekhead_entry));
    if (!entries)
        return AVERROR(ENOMEM);
    seekhead->entries = entries;

    seekhead->entries[seekhead->num_entries].elementid    = elementid;
    seekhead->entries[seekhead->num_entries++].segmentpos = filepos - seekhead->segment_offset;

    return 0;
}

/**
 * Write the seek head to the file and free it. If a maximum number of
 * elements was specified to mxv_start_seekhead(), the seek head will
 * be written at the location reserved for it. Otherwise, it is written
 * at the current location in the file.
 *
 * @return The file offset where the seekhead was written,
 * -1 if an error occurred.
 */
static int64_t mxv_write_seekhead(AVIOContext *pb, MXVMuxContext *mxv)
{
    AVIOContext *dyn_cp;
    mxv_seekhead *seekhead = mxv->seekhead;
    ebml_master seekentry;
    int64_t currentpos;
    int i;

    currentpos = avio_tell(pb);

    if (seekhead->reserved_size > 0) {
        if (avio_seek(pb, seekhead->filepos, SEEK_SET) < 0) {
            currentpos = -1;
            goto fail;
        }
    }

    if (start_ebml_master_crc32(pb, &dyn_cp, mxv, MXV_ID_SEEKHEAD) < 0) {
        currentpos = -1;
        goto fail;
    }

    for (i = 0; i < seekhead->num_entries; i++) {
        mxv_seekhead_entry *entry = &seekhead->entries[i];

        seekentry = start_ebml_master(dyn_cp, MXV_ID_SEEKENTRY, MAX_SEEKENTRY_SIZE);

        put_ebml_id(dyn_cp, MXV_ID_SEEKID);
        put_ebml_num(dyn_cp, ebml_id_size(entry->elementid), 0);
        put_ebml_id(dyn_cp, entry->elementid);

        put_ebml_uint(dyn_cp, MXV_ID_SEEKPOSITION, entry->segmentpos);
        end_ebml_master(dyn_cp, seekentry);
    }
    end_ebml_master_crc32(pb, &dyn_cp, mxv);

    if (seekhead->reserved_size > 0) {
        uint64_t remaining = seekhead->filepos + seekhead->reserved_size - avio_tell(pb);
        put_ebml_void(pb, remaining);
        avio_seek(pb, currentpos, SEEK_SET);

        currentpos = seekhead->filepos;
    }
fail:
    av_freep(&mxv->seekhead->entries);
    av_freep(&mxv->seekhead);

    return currentpos;
}

static mxv_cues *mxv_start_cues(int64_t segment_offset)
{
    mxv_cues *cues = av_mallocz(sizeof(mxv_cues));
    if (!cues)
        return NULL;

    cues->segment_offset = segment_offset;
    return cues;
}

static int mxv_add_cuepoint(mxv_cues *cues, int stream, int tracknum, int64_t ts,
                            int64_t cluster_pos, int64_t relative_pos, int64_t duration)
{
    mxv_cuepoint *entries = cues->entries;

    if (ts < 0)
        return 0;

    entries = av_realloc_array(entries, cues->num_entries + 1, sizeof(mxv_cuepoint));
    if (!entries)
        return AVERROR(ENOMEM);
    cues->entries = entries;

    cues->entries[cues->num_entries].pts           = ts;
    cues->entries[cues->num_entries].stream_idx    = stream;
    cues->entries[cues->num_entries].tracknum      = tracknum;
    cues->entries[cues->num_entries].cluster_pos   = cluster_pos - cues->segment_offset;
    cues->entries[cues->num_entries].relative_pos  = relative_pos;
    cues->entries[cues->num_entries++].duration    = duration;

    return 0;
}

static int64_t mxv_write_cues(AVFormatContext *s, mxv_cues *cues, mxv_track *tracks, int num_tracks)
{
    MXVMuxContext *mxv = s->priv_data;
    AVIOContext *dyn_cp, *pb = s->pb;
    int64_t currentpos;
    int i, j, ret;

    currentpos = avio_tell(pb);
    ret = start_ebml_master_crc32(pb, &dyn_cp, mxv, MXV_ID_CUES);
    if (ret < 0)
        return ret;

    for (i = 0; i < cues->num_entries; i++) {
        ebml_master cuepoint, track_positions;
        mxv_cuepoint *entry = &cues->entries[i];
        uint64_t pts = entry->pts;
        int ctp_nb = 0;

        // Calculate the number of entries, so we know the element size
        for (j = 0; j < num_tracks; j++)
            tracks[j].has_cue = 0;
        for (j = 0; j < cues->num_entries - i && entry[j].pts == pts; j++) {
            int tracknum = entry[j].stream_idx;
            av_assert0(tracknum>=0 && tracknum<num_tracks);
            if (tracks[tracknum].has_cue && s->streams[tracknum]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
                continue;
            tracks[tracknum].has_cue = 1;
            ctp_nb ++;
        }

        cuepoint = start_ebml_master(dyn_cp, MXV_ID_POINTENTRY, MAX_CUEPOINT_CONTENT_SIZE(ctp_nb));
        put_ebml_uint(dyn_cp, MXV_ID_CUETIME, pts);

        // put all the entries from different tracks that have the exact same
        // timestamp into the same CuePoint
        for (j = 0; j < num_tracks; j++)
            tracks[j].has_cue = 0;
        for (j = 0; j < cues->num_entries - i && entry[j].pts == pts; j++) {
            int tracknum = entry[j].stream_idx;
            av_assert0(tracknum>=0 && tracknum<num_tracks);
            if (tracks[tracknum].has_cue && s->streams[tracknum]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
                continue;
            tracks[tracknum].has_cue = 1;
            track_positions = start_ebml_master(dyn_cp, MXV_ID_CUETRACKPOSITION, MAX_CUETRACKPOS_SIZE);
            put_ebml_uint(dyn_cp, MXV_ID_CUETRACK           , entry[j].tracknum   );
            put_ebml_uint(dyn_cp, MXV_ID_CUECLUSTERPOSITION , entry[j].cluster_pos);
            put_ebml_uint(dyn_cp, MXV_ID_CUERELATIVEPOSITION, entry[j].relative_pos);
            if (entry[j].duration != -1)
                put_ebml_uint(dyn_cp, MXV_ID_CUEDURATION    , entry[j].duration);
            end_ebml_master(dyn_cp, track_positions);
        }
        i += j - 1;
        end_ebml_master(dyn_cp, cuepoint);
    }
    end_ebml_master_crc32(pb, &dyn_cp, mxv);

    return currentpos;
}

static int put_xiph_codecpriv(AVFormatContext *s, AVIOContext *pb, AVCodecParameters *par)
{
    const uint8_t *header_start[3];
    int header_len[3];
    int first_header_size;
    int j;

    if (par->codec_id == AV_CODEC_ID_VORBIS)
        first_header_size = 30;
    else
        first_header_size = 42;

    if (avpriv_split_xiph_headers(par->extradata, par->extradata_size,
                              first_header_size, header_start, header_len) < 0) {
        av_log(s, AV_LOG_ERROR, "Extradata corrupt.\n");
        return -1;
    }

    avio_w8(pb, 2);                    // number packets - 1
    for (j = 0; j < 2; j++) {
        put_xiph_size(pb, header_len[j]);
    }
    for (j = 0; j < 3; j++)
        avio_write(pb, header_start[j], header_len[j]);

    return 0;
}

static int put_wv_codecpriv(AVIOContext *pb, AVCodecParameters *par)
{
    if (par->extradata && par->extradata_size == 2)
        avio_write(pb, par->extradata, 2);
    else
        avio_wl16(pb, 0x403); // fallback to the version mentioned in matroska specs
    return 0;
}

static int put_flac_codecpriv(AVFormatContext *s,
                              AVIOContext *pb, AVCodecParameters *par)
{
    int write_comment = (par->channel_layout &&
                         !(par->channel_layout & ~0x3ffffULL) &&
                         !ff_flac_is_native_layout(par->channel_layout));
    int ret = ff_flac_write_header(pb, par->extradata, par->extradata_size,
                                   !write_comment);

    if (ret < 0)
        return ret;

    if (write_comment) {
        const char *vendor = (s->flags & AVFMT_FLAG_BITEXACT) ?
                             "Lavf" : LIBAVFORMAT_IDENT;
        AVDictionary *dict = NULL;
        uint8_t buf[32], *data, *p;
        int64_t len;

        snprintf(buf, sizeof(buf), "0x%"PRIx64, par->channel_layout);
        av_dict_set(&dict, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK", buf, 0);

        len = ff_vorbiscomment_length(dict, vendor, NULL, 0);
        if (len >= ((1<<24) - 4)) {
            av_dict_free(&dict);
            return AVERROR(EINVAL);
        }

        data = av_malloc(len + 4);
        if (!data) {
            av_dict_free(&dict);
            return AVERROR(ENOMEM);
        }

        data[0] = 0x84;
        AV_WB24(data + 1, len);

        p = data + 4;
        ff_vorbiscomment_write(&p, &dict, vendor, NULL, 0);

        avio_write(pb, data, len + 4);

        av_freep(&data);
        av_dict_free(&dict);
    }

    return 0;
}

static int get_aac_sample_rates(AVFormatContext *s, uint8_t *extradata, int extradata_size,
                                int *sample_rate, int *output_sample_rate)
{
    MPEG4AudioConfig mp4ac;
    int ret;

    ret = avpriv_mpeg4audio_get_config(&mp4ac, extradata,
                                       extradata_size * 8, 1);
    /* Don't abort if the failure is because of missing extradata. Assume in that
     * case a bitstream filter will provide the muxer with the extradata in the
     * first packet.
     * Abort however if s->pb is not seekable, as we would not be able to seek back
     * to write the sample rate elements once the extradata shows up, anyway. */
    if (ret < 0 && (extradata_size || !(s->pb->seekable & AVIO_SEEKABLE_NORMAL))) {
        av_log(s, AV_LOG_ERROR,
               "Error parsing AAC extradata, unable to determine samplerate.\n");
        return AVERROR(EINVAL);
    }

    if (ret < 0) {
        /* This will only happen when this function is called while writing the
         * header and no extradata is available. The space for this element has
         * to be reserved for when this function is called again after the
         * extradata shows up in the first packet, as there's no way to know if
         * output_sample_rate will be different than sample_rate or not. */
        *output_sample_rate = *sample_rate;
    } else {
        *sample_rate        = mp4ac.sample_rate;
        *output_sample_rate = mp4ac.ext_sample_rate;
    }
    return 0;
}

static int mxv_write_native_codecprivate(AVFormatContext *s, AVIOContext *pb,
                                         AVCodecParameters *par,
                                         AVIOContext *dyn_cp)
{
    switch (par->codec_id) {
    case AV_CODEC_ID_VORBIS:
    case AV_CODEC_ID_THEORA:
        return put_xiph_codecpriv(s, dyn_cp, par);
    case AV_CODEC_ID_FLAC:
        return put_flac_codecpriv(s, dyn_cp, par);
    case AV_CODEC_ID_WAVPACK:
        return put_wv_codecpriv(dyn_cp, par);
    case AV_CODEC_ID_H264:
        return ff_isom_write_avcc(dyn_cp, par->extradata,
                                  par->extradata_size);
    case AV_CODEC_ID_HEVC:
        ff_isom_write_hvcc(dyn_cp, par->extradata,
                           par->extradata_size, 0);
        return 0;
    case AV_CODEC_ID_AV1:
        if (par->extradata_size)
            return ff_isom_write_av1c(dyn_cp, par->extradata,
                                      par->extradata_size);
        else
            put_ebml_void(pb, 4 + 3);
        break;
    case AV_CODEC_ID_ALAC:
        if (par->extradata_size < 36) {
            av_log(s, AV_LOG_ERROR,
                   "Invalid extradata found, ALAC expects a 36-byte "
                   "QuickTime atom.");
            return AVERROR_INVALIDDATA;
        } else
            avio_write(dyn_cp, par->extradata + 12,
                       par->extradata_size - 12);
        break;
    case AV_CODEC_ID_AAC:
        if (par->extradata_size)
            avio_write(dyn_cp, par->extradata, par->extradata_size);
        else
            put_ebml_void(pb, MAX_PCE_SIZE + 2 + 4);
        break;
    default:
        if (par->codec_id == AV_CODEC_ID_PRORES &&
            ff_codec_get_id(ff_codec_movvideo_tags, par->codec_tag) == AV_CODEC_ID_PRORES) {
            avio_wl32(dyn_cp, par->codec_tag);
        } else if (par->extradata_size && par->codec_id != AV_CODEC_ID_TTA)
            avio_write(dyn_cp, par->extradata, par->extradata_size);
    }

    return 0;
}

static int mxv_write_codecprivate(AVFormatContext *s, AVIOContext *pb,
                                  AVCodecParameters *par,
                                  int native_id, int qt_id)
{
    AVIOContext *dyn_cp;
    uint8_t *codecpriv;
    int ret, codecpriv_size;

    ret = avio_open_dyn_buf(&dyn_cp);
    if (ret < 0)
        return ret;

    if (native_id) {
        ret = mxv_write_native_codecprivate(s, pb, par, dyn_cp);
    } else if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (qt_id) {
            if (!par->codec_tag)
                par->codec_tag = ff_codec_get_tag(ff_codec_movvideo_tags,
                                                    par->codec_id);
            if (   ff_codec_get_id(ff_codec_movvideo_tags, par->codec_tag) == par->codec_id
                && (!par->extradata_size || ff_codec_get_id(ff_codec_movvideo_tags, AV_RL32(par->extradata + 4)) != par->codec_id)
            ) {
                int i;
                avio_wb32(dyn_cp, 0x5a + par->extradata_size);
                avio_wl32(dyn_cp, par->codec_tag);
                for(i = 0; i < 0x5a - 8; i++)
                    avio_w8(dyn_cp, 0);
            }
            avio_write(dyn_cp, par->extradata, par->extradata_size);
        } else {
            if (!ff_codec_get_tag(ff_codec_bmp_tags, par->codec_id))
                av_log(s, AV_LOG_WARNING, "codec %s is not supported by this format\n",
                       avcodec_get_name(par->codec_id));

            if (!par->codec_tag)
                par->codec_tag = ff_codec_get_tag(ff_codec_bmp_tags,
                                                  par->codec_id);
            if (!par->codec_tag && par->codec_id != AV_CODEC_ID_RAWVIDEO) {
                av_log(s, AV_LOG_ERROR, "No bmp codec tag found for codec %s\n",
                       avcodec_get_name(par->codec_id));
                ret = AVERROR(EINVAL);
            }

            ff_put_bmp_header(dyn_cp, par, 0, 0);
        }
    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
        unsigned int tag;
        tag = ff_codec_get_tag(ff_codec_wav_tags, par->codec_id);
        if (!tag) {
            av_log(s, AV_LOG_ERROR, "No wav codec tag found for codec %s\n",
                   avcodec_get_name(par->codec_id));
            ret = AVERROR(EINVAL);
        }
        if (!par->codec_tag)
            par->codec_tag = tag;

        ff_put_wav_header(s, dyn_cp, par, FF_PUT_WAV_HEADER_FORCE_WAVEFORMATEX);
    }

    codecpriv_size = avio_close_dyn_buf(dyn_cp, &codecpriv);
    if (codecpriv_size)
        put_ebml_binary(pb, MXV_ID_CODECPRIVATE, codecpriv,
                        codecpriv_size);
    av_free(codecpriv);
    return ret;
}

static int mxv_write_video_color(AVIOContext *pb, AVCodecParameters *par, AVStream *st) {
    AVIOContext *dyn_cp;
    uint8_t *colorinfo_ptr;
    int side_data_size = 0;
    int ret, colorinfo_size;
    const uint8_t *side_data;

    ret = avio_open_dyn_buf(&dyn_cp);
    if (ret < 0)
        return ret;

    if (par->color_trc != AVCOL_TRC_UNSPECIFIED &&
        par->color_trc < AVCOL_TRC_NB) {
        put_ebml_uint(dyn_cp, MXV_ID_VIDEOCOLORTRANSFERCHARACTERISTICS,
                      par->color_trc);
    }
    if (par->color_space != AVCOL_SPC_UNSPECIFIED &&
        par->color_space < AVCOL_SPC_NB) {
        put_ebml_uint(dyn_cp, MXV_ID_VIDEOCOLORMATRIXCOEFF, par->color_space);
    }
    if (par->color_primaries != AVCOL_PRI_UNSPECIFIED &&
        par->color_primaries < AVCOL_PRI_NB) {
        put_ebml_uint(dyn_cp, MXV_ID_VIDEOCOLORPRIMARIES, par->color_primaries);
    }
    if (par->color_range != AVCOL_RANGE_UNSPECIFIED &&
        par->color_range < AVCOL_RANGE_NB) {
        put_ebml_uint(dyn_cp, MXV_ID_VIDEOCOLORRANGE, par->color_range);
    }
    if (par->chroma_location != AVCHROMA_LOC_UNSPECIFIED &&
        par->chroma_location <= AVCHROMA_LOC_TOP) {
        int xpos, ypos;

        avcodec_enum_to_chroma_pos(&xpos, &ypos, par->chroma_location);
        put_ebml_uint(dyn_cp, MXV_ID_VIDEOCOLORCHROMASITINGHORZ, (xpos >> 7) + 1);
        put_ebml_uint(dyn_cp, MXV_ID_VIDEOCOLORCHROMASITINGVERT, (ypos >> 7) + 1);
    }

    side_data = av_stream_get_side_data(st, AV_PKT_DATA_CONTENT_LIGHT_LEVEL,
                                        &side_data_size);
    if (side_data_size) {
        const AVContentLightMetadata *metadata =
            (const AVContentLightMetadata*)side_data;
        put_ebml_uint(dyn_cp, MXV_ID_VIDEOCOLORMAXCLL,  metadata->MaxCLL);
        put_ebml_uint(dyn_cp, MXV_ID_VIDEOCOLORMAXFALL, metadata->MaxFALL);
    }

    side_data = av_stream_get_side_data(st, AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
                                        &side_data_size);
    if (side_data_size == sizeof(AVMasteringDisplayMetadata)) {
        ebml_master meta_element = start_ebml_master(
            dyn_cp, MXV_ID_VIDEOCOLORMASTERINGMETA, 0);
        const AVMasteringDisplayMetadata *metadata =
            (const AVMasteringDisplayMetadata*)side_data;
        if (metadata->has_primaries) {
            put_ebml_float(dyn_cp, MXV_ID_VIDEOCOLOR_RX,
                           av_q2d(metadata->display_primaries[0][0]));
            put_ebml_float(dyn_cp, MXV_ID_VIDEOCOLOR_RY,
                           av_q2d(metadata->display_primaries[0][1]));
            put_ebml_float(dyn_cp, MXV_ID_VIDEOCOLOR_GX,
                           av_q2d(metadata->display_primaries[1][0]));
            put_ebml_float(dyn_cp, MXV_ID_VIDEOCOLOR_GY,
                           av_q2d(metadata->display_primaries[1][1]));
            put_ebml_float(dyn_cp, MXV_ID_VIDEOCOLOR_BX,
                           av_q2d(metadata->display_primaries[2][0]));
            put_ebml_float(dyn_cp, MXV_ID_VIDEOCOLOR_BY,
                           av_q2d(metadata->display_primaries[2][1]));
            put_ebml_float(dyn_cp, MXV_ID_VIDEOCOLOR_WHITEX,
                           av_q2d(metadata->white_point[0]));
            put_ebml_float(dyn_cp, MXV_ID_VIDEOCOLOR_WHITEY,
                           av_q2d(metadata->white_point[1]));
        }
        if (metadata->has_luminance) {
            put_ebml_float(dyn_cp, MXV_ID_VIDEOCOLOR_LUMINANCEMAX,
                           av_q2d(metadata->max_luminance));
            put_ebml_float(dyn_cp, MXV_ID_VIDEOCOLOR_LUMINANCEMIN,
                           av_q2d(metadata->min_luminance));
        }
        end_ebml_master(dyn_cp, meta_element);
    }

    colorinfo_size = avio_close_dyn_buf(dyn_cp, &colorinfo_ptr);
    if (colorinfo_size) {
        ebml_master colorinfo = start_ebml_master(pb, MXV_ID_VIDEOCOLOR, colorinfo_size);
        avio_write(pb, colorinfo_ptr, colorinfo_size);
        end_ebml_master(pb, colorinfo);
    }
    av_free(colorinfo_ptr);
    return 0;
}

static int mxv_write_video_projection(AVFormatContext *s, AVIOContext *pb,
                                      AVStream *st)
{
    AVIOContext b;
    AVIOContext *dyn_cp;
    int side_data_size = 0;
    int ret, projection_size;
    uint8_t *projection_ptr;
    uint8_t private[20];

    const AVSphericalMapping *spherical =
        (const AVSphericalMapping *)av_stream_get_side_data(st, AV_PKT_DATA_SPHERICAL,
                                                            &side_data_size);

    if (!side_data_size)
        return 0;

    ret = avio_open_dyn_buf(&dyn_cp);
    if (ret < 0)
        return ret;

    switch (spherical->projection) {
    case AV_SPHERICAL_EQUIRECTANGULAR:
        put_ebml_uint(dyn_cp, MXV_ID_VIDEOPROJECTIONTYPE,
                      MXV_VIDEO_PROJECTION_TYPE_EQUIRECTANGULAR);
        break;
    case AV_SPHERICAL_EQUIRECTANGULAR_TILE:
        ffio_init_context(&b, private, 20, 1, NULL, NULL, NULL, NULL);
        put_ebml_uint(dyn_cp, MXV_ID_VIDEOPROJECTIONTYPE,
                      MXV_VIDEO_PROJECTION_TYPE_EQUIRECTANGULAR);
        avio_wb32(&b, 0); // version + flags
        avio_wb32(&b, spherical->bound_top);
        avio_wb32(&b, spherical->bound_bottom);
        avio_wb32(&b, spherical->bound_left);
        avio_wb32(&b, spherical->bound_right);
        put_ebml_binary(dyn_cp, MXV_ID_VIDEOPROJECTIONPRIVATE,
                        private, avio_tell(&b));
        break;
    case AV_SPHERICAL_CUBEMAP:
        ffio_init_context(&b, private, 12, 1, NULL, NULL, NULL, NULL);
        put_ebml_uint(dyn_cp, MXV_ID_VIDEOPROJECTIONTYPE,
                      MXV_VIDEO_PROJECTION_TYPE_CUBEMAP);
        avio_wb32(&b, 0); // version + flags
        avio_wb32(&b, 0); // layout
        avio_wb32(&b, spherical->padding);
        put_ebml_binary(dyn_cp, MXV_ID_VIDEOPROJECTIONPRIVATE,
                        private, avio_tell(&b));
        break;
    default:
        av_log(s, AV_LOG_WARNING, "Unknown projection type\n");
        goto end;
    }

    if (spherical->yaw)
        put_ebml_float(dyn_cp, MXV_ID_VIDEOPROJECTIONPOSEYAW,
                       (double) spherical->yaw   / (1 << 16));
    if (spherical->pitch)
        put_ebml_float(dyn_cp, MXV_ID_VIDEOPROJECTIONPOSEPITCH,
                       (double) spherical->pitch / (1 << 16));
    if (spherical->roll)
        put_ebml_float(dyn_cp, MXV_ID_VIDEOPROJECTIONPOSEROLL,
                       (double) spherical->roll  / (1 << 16));

end:
    projection_size = avio_close_dyn_buf(dyn_cp, &projection_ptr);
    if (projection_size) {
        ebml_master projection = start_ebml_master(pb,
                                                   MXV_ID_VIDEOPROJECTION,
                                                   projection_size);
        avio_write(pb, projection_ptr, projection_size);
        end_ebml_master(pb, projection);
    }
    av_freep(&projection_ptr);

    return 0;
}
static int mxv_write_content_encodings(AVFormatContext *s, AVIOContext *pb,
                                      AVStream *st)
{

    MXVMuxContext *mxv = s->priv_data;
    ebml_master encodings, encoding;
    size_t b64_size = AV_BASE64_SIZE(TRACK_ENCRYPTION_KEY_SIZE);
    char* b64EcnKey = av_malloc(b64_size);
    av_base64_encode(b64EcnKey, b64_size, mxv->aes_key, TRACK_ENCRYPTION_KEY_SIZE);

    encodings = start_ebml_master(pb, MXV_ID_TRACKCONTENTENCODINGS, 0);
    encoding = start_ebml_master(pb, MXV_ID_TRACKCONTENTENCODING, 0);
    put_ebml_uint(pb, MXV_ID_ENCODINGORDER, 0);
    put_ebml_uint(pb, MXV_ID_ENCODINGSCOPE, 1);
    put_ebml_uint(pb, MXV_ID_ENCODINGTYPE, 1);
        //encoding type is 0
//        ebml_master compression = start_ebml_master(pb, MXV_ID_ENCODINGCOMPRESSION, 0);
//        /* encoding comp algo has
//        0 - zlib, default
//        1 - bzlib,
//        2 - lzo1x,
//        3 - Header Stripping */
//        put_ebml_uint(pb, MXV_ID_ENCODINGCOMPALGO, MXV_TRACK_ENCODING_COMP_ZLIB);
////        put_ebml_binary(pb, MXV_ID_ENCODINGCOMPSETTINGS, NULL, 1);
//        end_ebml_master(pb, compression);
        //encoding type is 1
        ebml_master encryption = start_ebml_master(pb, MXV_ID_ENCODINGENCRYPTION, 0);
        /*
        0 - Not encrypted, default
        1 - DES - FIPS 46-3,
        2 - Triple DES - RFC 1851,
        3 - Twofish,
        4 - Blowfish,
        5 - AES - FIPS 187 */
        put_ebml_uint(pb, MXV_ID_ENCODINGENCALGO, MXV_TRACK_ENCODING_ENC_AES);
        put_ebml_binary(pb, MXV_ID_ENCODINGENCKEYID, b64EcnKey, b64_size);
//            //ecoding algo is 5 - AES - FIPS 187
//            ebml_master AESSettinng = start_ebml_master(pb, MXV_ID_ENCODINGENCAESSETTINGS, 0);
//            put_ebml_binary(pb, MXV_ID_ENCODINGENCAESSettingsCipherMode, NULL, 1);
//            end_ebml_master(pb, AESSettinng);
//        put_ebml_binary(pb, MXV_ID_ENCODINGSIGNATURE, NULL, 1);
//        put_ebml_binary(pb, MXV_ID_ENCODINGSIGKEYID, NULL, 1);
//        /* encoding sign algo
//        0 - Not signed, default
//        1 - RSA */
//        put_ebml_uint(pb, MXV_ID_ENCODINGSIGALGO, MXV_TRACK_ENCODING_ENC_CONTENT_SIGN_NONE);
//        /*    The hash algorithm used for the signature.
//        0 - Not signed, default
//        1 - SHA1-160,
//        1 - MD5 */
//        put_ebml_uint(pb, MXV_ID_ENCODINGSIGHASHALGO, MXV_TRACK_ENCODING_ENC_CONTENT_SIGN_HASH_NONE);
        end_ebml_master(pb, encryption);

    end_ebml_master(pb, encoding);
    end_ebml_master(pb, encodings);

    av_free(b64EcnKey);

    return 0;
}

static void mxv_write_field_order(AVIOContext *pb, int mode,
                                  enum AVFieldOrder field_order)
{
    switch (field_order) {
    case AV_FIELD_UNKNOWN:
        break;
    case AV_FIELD_PROGRESSIVE:
        put_ebml_uint(pb, MXV_ID_VIDEOFLAGINTERLACED,
                      MXV_VIDEO_INTERLACE_FLAG_PROGRESSIVE);
        break;
    case AV_FIELD_TT:
    case AV_FIELD_BB:
    case AV_FIELD_TB:
    case AV_FIELD_BT:
        put_ebml_uint(pb, MXV_ID_VIDEOFLAGINTERLACED,
                      MXV_VIDEO_INTERLACE_FLAG_INTERLACED);
        if (mode != MODE_WEBM) {
            switch (field_order) {
            case AV_FIELD_TT:
                put_ebml_uint(pb, MXV_ID_VIDEOFIELDORDER,
                              MXV_VIDEO_FIELDORDER_TT);
                break;
            case AV_FIELD_BB:
                put_ebml_uint(pb, MXV_ID_VIDEOFIELDORDER,
                              MXV_VIDEO_FIELDORDER_BB);
                break;
            case AV_FIELD_TB:
                put_ebml_uint(pb, MXV_ID_VIDEOFIELDORDER,
                              MXV_VIDEO_FIELDORDER_TB);
                break;
            case AV_FIELD_BT:
                put_ebml_uint(pb, MXV_ID_VIDEOFIELDORDER,
                              MXV_VIDEO_FIELDORDER_BT);
                break;
            }
        }
    }
}

static int mxv_write_stereo_mode(AVFormatContext *s, AVIOContext *pb,
                                 AVStream *st, int mode, int *h_width, int *h_height)
{
    int i;
    int ret = 0;
    AVDictionaryEntry *tag;
    MXVVideoStereoModeType format = MXV_VIDEO_STEREOMODE_TYPE_NB;

    *h_width = 1;
    *h_height = 1;
    // convert metadata into proper side data and add it to the stream
    if ((tag = av_dict_get(st->metadata, "stereo_mode", NULL, 0)) ||
        (tag = av_dict_get( s->metadata, "stereo_mode", NULL, 0))) {
        int stereo_mode = atoi(tag->value);

        for (i=0; i<MXV_VIDEO_STEREOMODE_TYPE_NB; i++)
            if (!strcmp(tag->value, ff_mxv_video_stereo_mode[i])){
                stereo_mode = i;
                break;
            }

        if (stereo_mode < MXV_VIDEO_STEREOMODE_TYPE_NB &&
            stereo_mode != 10 && stereo_mode != 12) {
            int ret = ff_mxv_stereo3d_conv(st, stereo_mode);
            if (ret < 0)
                return ret;
        }
    }

    // iterate to find the stereo3d side data
    for (i = 0; i < st->nb_side_data; i++) {
        AVPacketSideData sd = st->side_data[i];
        if (sd.type == AV_PKT_DATA_STEREO3D) {
            AVStereo3D *stereo = (AVStereo3D *)sd.data;

            switch (stereo->type) {
            case AV_STEREO3D_2D:
                format = MXV_VIDEO_STEREOMODE_TYPE_MONO;
                break;
            case AV_STEREO3D_SIDEBYSIDE:
                format = (stereo->flags & AV_STEREO3D_FLAG_INVERT)
                    ? MXV_VIDEO_STEREOMODE_TYPE_RIGHT_LEFT
                    : MXV_VIDEO_STEREOMODE_TYPE_LEFT_RIGHT;
                *h_width = 2;
                break;
            case AV_STEREO3D_TOPBOTTOM:
                format = MXV_VIDEO_STEREOMODE_TYPE_TOP_BOTTOM;
                if (stereo->flags & AV_STEREO3D_FLAG_INVERT)
                    format--;
                *h_height = 2;
                break;
            case AV_STEREO3D_CHECKERBOARD:
                format = MXV_VIDEO_STEREOMODE_TYPE_CHECKERBOARD_LR;
                if (stereo->flags & AV_STEREO3D_FLAG_INVERT)
                    format--;
                break;
            case AV_STEREO3D_LINES:
                format = MXV_VIDEO_STEREOMODE_TYPE_ROW_INTERLEAVED_LR;
                if (stereo->flags & AV_STEREO3D_FLAG_INVERT)
                    format--;
                *h_height = 2;
                break;
            case AV_STEREO3D_COLUMNS:
                format = MXV_VIDEO_STEREOMODE_TYPE_COL_INTERLEAVED_LR;
                if (stereo->flags & AV_STEREO3D_FLAG_INVERT)
                    format--;
                *h_width = 2;
                break;
            case AV_STEREO3D_FRAMESEQUENCE:
                format = MXV_VIDEO_STEREOMODE_TYPE_BOTH_EYES_BLOCK_LR;
                if (stereo->flags & AV_STEREO3D_FLAG_INVERT)
                    format++;
                break;
            }
            break;
        }
    }

    if (format == MXV_VIDEO_STEREOMODE_TYPE_NB)
        return ret;

    // if webm, do not write unsupported modes
    if ((mode == MODE_WEBM &&
        format > MXV_VIDEO_STEREOMODE_TYPE_TOP_BOTTOM &&
        format != MXV_VIDEO_STEREOMODE_TYPE_RIGHT_LEFT)
        || format >= MXV_VIDEO_STEREOMODE_TYPE_NB) {
        av_log(s, AV_LOG_ERROR,
               "The specified stereo mode is not valid.\n");
        format = MXV_VIDEO_STEREOMODE_TYPE_NB;
        return AVERROR(EINVAL);
    }

    // write StereoMode if format is valid
    put_ebml_uint(pb, MXV_ID_VIDEOSTEREOMODE, format);

    return ret;
}

static int mxv_write_track(AVFormatContext *s, MXVMuxContext *mxv,
                           int i, AVIOContext *pb, int default_stream_exists)
{
    AVStream *st = s->streams[i];
    AVCodecParameters *par = st->codecpar;
    ebml_master subinfo, track;
    int native_id = 0;
    int qt_id = 0;
    int bit_depth = av_get_bits_per_sample(par->codec_id);
    int sample_rate = par->sample_rate;
    int output_sample_rate = 0;
    int display_width_div = 1;
    int display_height_div = 1;
    int j, ret;
    AVDictionaryEntry *tag;

    if (par->codec_type == AVMEDIA_TYPE_ATTACHMENT) {
        mxv->have_attachments = 1;
        return 0;
    }

    if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
        if (!bit_depth && par->codec_id != AV_CODEC_ID_ADPCM_G726) {
            if (par->bits_per_raw_sample)
                bit_depth = par->bits_per_raw_sample;
            else
                bit_depth = av_get_bytes_per_sample(par->format) << 3;
        }
        if (!bit_depth)
            bit_depth = par->bits_per_coded_sample;
    }

    if (par->codec_id == AV_CODEC_ID_AAC) {
        ret = get_aac_sample_rates(s, par->extradata, par->extradata_size, &sample_rate,
                                   &output_sample_rate);
        if (ret < 0)
            return ret;
    }

    track = start_ebml_master(pb, MXV_ID_TRACKENTRY, 0);
    put_ebml_uint (pb, MXV_ID_TRACKNUMBER,
                   mxv->is_dash ? mxv->dash_track_number : i + 1);
    put_ebml_uint (pb, MXV_ID_TRACKUID,
                   mxv->is_dash ? mxv->dash_track_number : i + 1);
    put_ebml_uint (pb, MXV_ID_TRACKFLAGLACING , 0);    // no lacing (yet)

    if ((tag = av_dict_get(st->metadata, "title", NULL, 0)))
        put_ebml_string(pb, MXV_ID_TRACKNAME, tag->value);
    tag = av_dict_get(st->metadata, "language", NULL, 0);
    if (mxv->mode != MODE_WEBM || par->codec_id != AV_CODEC_ID_WEBVTT) {
        put_ebml_string(pb, MXV_ID_TRACKLANGUAGE, tag && tag->value ? tag->value:"und");
    } else if (tag && tag->value) {
        put_ebml_string(pb, MXV_ID_TRACKLANGUAGE, tag->value);
    }

    // The default value for TRACKFLAGDEFAULT is 1, so add element
    // if we need to clear it.
    if (default_stream_exists && !(st->disposition & AV_DISPOSITION_DEFAULT))
        put_ebml_uint(pb, MXV_ID_TRACKFLAGDEFAULT, !!(st->disposition & AV_DISPOSITION_DEFAULT));

    if (st->disposition & AV_DISPOSITION_FORCED)
        put_ebml_uint(pb, MXV_ID_TRACKFLAGFORCED, 1);

//    if (mxv->mode == MODE_WEBM) {
//        const char *codec_id;
//        if (par->codec_type != AVMEDIA_TYPE_SUBTITLE) {
//            for (j = 0; ff_webm_codec_tags[j].id != AV_CODEC_ID_NONE; j++) {
//                if (ff_webm_codec_tags[j].id == par->codec_id) {
//                    codec_id = ff_webm_codec_tags[j].str;
//                    native_id = 1;
//                    break;
//                }
//            }
//        } else if (par->codec_id == AV_CODEC_ID_WEBVTT) {
//            if (st->disposition & AV_DISPOSITION_CAPTIONS) {
//                codec_id = "D_WEBVTT/CAPTIONS";
//                native_id = MXV_TRACK_TYPE_SUBTITLE;
//            } else if (st->disposition & AV_DISPOSITION_DESCRIPTIONS) {
//                codec_id = "D_WEBVTT/DESCRIPTIONS";
//                native_id = MXV_TRACK_TYPE_METADATA;
//            } else if (st->disposition & AV_DISPOSITION_METADATA) {
//                codec_id = "D_WEBVTT/METADATA";
//                native_id = MXV_TRACK_TYPE_METADATA;
//            } else {
//                codec_id = "D_WEBVTT/SUBTITLES";
//                native_id = MXV_TRACK_TYPE_SUBTITLE;
//            }
//        }
//
//        if (!native_id) {
//            av_log(s, AV_LOG_ERROR,
//                   "Only VP8 or VP9 or AV1 video and Vorbis or Opus audio and WebVTT subtitles are supported for WebM.\n");
//            return AVERROR(EINVAL);
//        }
//
//        put_ebml_string(pb, MXV_ID_CODECID, codec_id);
//    } else {
        // look for a codec ID string specific to mxv to use,
        // if none are found, use AVI codes
        if (par->codec_id != AV_CODEC_ID_RAWVIDEO || par->codec_tag) {
            for (j = 0; ff_mxv_codec_tags[j].id != AV_CODEC_ID_NONE; j++) {
                if (ff_mxv_codec_tags[j].id == par->codec_id && par->codec_id != AV_CODEC_ID_FFV1) {
                    put_ebml_string(pb, MXV_ID_CODECID, ff_mxv_codec_tags[j].str);
                    native_id = 1;
                    break;
                }
            }
        } else {
            if (mxv->allow_raw_vfw) {
                native_id = 0;
            } else {
                av_log(s, AV_LOG_ERROR, "Raw RGB is not supported Natively in MXV, you can use AVI or NUT or\n"
                                        "If you would like to store it anyway using VFW mode, enable allow_raw_vfw (-allow_raw_vfw 1)\n");
                return AVERROR(EINVAL);
            }
        }
//    }

    if (par->codec_type == AVMEDIA_TYPE_AUDIO && par->initial_padding && par->codec_id == AV_CODEC_ID_OPUS) {
        int64_t codecdelay = av_rescale_q(par->initial_padding,
                                          (AVRational){ 1, 48000 },
                                          (AVRational){ 1, 1000000000 });
        if (codecdelay < 0) {
            av_log(s, AV_LOG_ERROR, "Initial padding is invalid\n");
            return AVERROR(EINVAL);
        }
//         mxv->tracks[i].ts_offset = av_rescale_q(par->initial_padding,
//                                                 (AVRational){ 1, par->sample_rate },
//                                                 st->time_base);

        put_ebml_uint(pb, MXV_ID_CODECDELAY, codecdelay);
    }
    if (par->codec_id == AV_CODEC_ID_OPUS) {
        put_ebml_uint(pb, MXV_ID_SEEKPREROLL, OPUS_SEEK_PREROLL);
    }
    
    ret = mxv_write_content_encodings(s, pb, st);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "write mxv encodings fail");
    }

    switch (par->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        mxv->have_video = 1;
        put_ebml_uint(pb, MXV_ID_TRACKTYPE, MXV_TRACK_TYPE_VIDEO);

        if(   st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0
           && av_cmp_q(av_inv_q(st->avg_frame_rate), st->time_base) > 0)
            put_ebml_uint(pb, MXV_ID_TRACKDEFAULTDURATION, 1000000000LL * st->avg_frame_rate.den / st->avg_frame_rate.num);

        if (!native_id &&
            ff_codec_get_tag(ff_codec_movvideo_tags, par->codec_id) &&
            ((!ff_codec_get_tag(ff_codec_bmp_tags,   par->codec_id) && par->codec_id != AV_CODEC_ID_RAWVIDEO) ||
             par->codec_id == AV_CODEC_ID_SVQ1 ||
             par->codec_id == AV_CODEC_ID_SVQ3 ||
             par->codec_id == AV_CODEC_ID_CINEPAK))
            qt_id = 1;

        if (qt_id)
            put_ebml_string(pb, MXV_ID_CODECID, "V_QUICKTIME");
        else if (!native_id) {
            // if there is no mxv-specific codec ID, use VFW mode
            put_ebml_string(pb, MXV_ID_CODECID, "V_MS/VFW/FOURCC");
            mxv->tracks[i].write_dts = 1;
            s->internal->avoid_negative_ts_use_pts = 0;
        }

        subinfo = start_ebml_master(pb, MXV_ID_TRACKVIDEO, 0);

        put_ebml_uint (pb, MXV_ID_VIDEOPIXELWIDTH , par->width);
        put_ebml_uint (pb, MXV_ID_VIDEOPIXELHEIGHT, par->height);

        mxv_write_field_order(pb, mxv->mode, par->field_order);

        // check both side data and metadata for stereo information,
        // write the result to the bitstream if any is found
        ret = mxv_write_stereo_mode(s, pb, st, mxv->mode,
                                    &display_width_div,
                                    &display_height_div);
        if (ret < 0)
            return ret;

        if (((tag = av_dict_get(st->metadata, "alpha_mode", NULL, 0)) && atoi(tag->value)) ||
            ((tag = av_dict_get( s->metadata, "alpha_mode", NULL, 0)) && atoi(tag->value)) ||
            (par->format == AV_PIX_FMT_YUVA420P)) {
            put_ebml_uint(pb, MXV_ID_VIDEOALPHAMODE, 1);
        }

        // write DisplayWidth and DisplayHeight, they contain the size of
        // a single source view and/or the display aspect ratio
        if (st->sample_aspect_ratio.num) {
            int64_t d_width = av_rescale(par->width, st->sample_aspect_ratio.num, st->sample_aspect_ratio.den);
            if (d_width > INT_MAX) {
                av_log(s, AV_LOG_ERROR, "Overflow in display width\n");
                return AVERROR(EINVAL);
            }
            if (d_width != par->width || display_width_div != 1 || display_height_div != 1) {
                if (mxv->mode == MODE_WEBM || display_width_div != 1 || display_height_div != 1) {
                    put_ebml_uint(pb, MXV_ID_VIDEODISPLAYWIDTH , d_width / display_width_div);
                    put_ebml_uint(pb, MXV_ID_VIDEODISPLAYHEIGHT, par->height / display_height_div);
                } else {
                    AVRational display_aspect_ratio;
                    av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
                              par->width  * (int64_t)st->sample_aspect_ratio.num,
                              par->height * (int64_t)st->sample_aspect_ratio.den,
                              1024 * 1024);
                    put_ebml_uint(pb, MXV_ID_VIDEODISPLAYWIDTH,  display_aspect_ratio.num);
                    put_ebml_uint(pb, MXV_ID_VIDEODISPLAYHEIGHT, display_aspect_ratio.den);
                    put_ebml_uint(pb, MXV_ID_VIDEODISPLAYUNIT, MXV_VIDEO_DISPLAYUNIT_DAR);
                }
            }
        } else if (display_width_div != 1 || display_height_div != 1) {
            put_ebml_uint(pb, MXV_ID_VIDEODISPLAYWIDTH , par->width / display_width_div);
            put_ebml_uint(pb, MXV_ID_VIDEODISPLAYHEIGHT, par->height / display_height_div);
        } else if (mxv->mode != MODE_WEBM)
            put_ebml_uint(pb, MXV_ID_VIDEODISPLAYUNIT, MXV_VIDEO_DISPLAYUNIT_UNKNOWN);

        if (par->codec_id == AV_CODEC_ID_RAWVIDEO) {
            uint32_t color_space = av_le2ne32(par->codec_tag);
            put_ebml_binary(pb, MXV_ID_VIDEOCOLORSPACE, &color_space, sizeof(color_space));
        }
        ret = mxv_write_video_color(pb, par, st);
        if (ret < 0)
            return ret;
        ret = mxv_write_video_projection(s, pb, st);
        if (ret < 0)
            return ret;
        end_ebml_master(pb, subinfo);
        break;

    case AVMEDIA_TYPE_AUDIO:
        put_ebml_uint(pb, MXV_ID_TRACKTYPE, MXV_TRACK_TYPE_AUDIO);

        if (!native_id)
            // no mxv-specific ID, use ACM mode
            put_ebml_string(pb, MXV_ID_CODECID, "A_MS/ACM");

        subinfo = start_ebml_master(pb, MXV_ID_TRACKAUDIO, 0);
        put_ebml_uint  (pb, MXV_ID_AUDIOCHANNELS    , par->channels);

        mxv->tracks[i].sample_rate_offset = avio_tell(pb);
        put_ebml_float (pb, MXV_ID_AUDIOSAMPLINGFREQ, sample_rate);
        if (output_sample_rate)
            put_ebml_float(pb, MXV_ID_AUDIOOUTSAMPLINGFREQ, output_sample_rate);
        if (bit_depth)
            put_ebml_uint(pb, MXV_ID_AUDIOBITDEPTH, bit_depth);
        end_ebml_master(pb, subinfo);
        break;

    case AVMEDIA_TYPE_SUBTITLE:
        if (!native_id) {
            av_log(s, AV_LOG_ERROR, "Subtitle codec %d is not supported.\n", par->codec_id);
            return AVERROR(ENOSYS);
        }

        if (mxv->mode != MODE_WEBM || par->codec_id != AV_CODEC_ID_WEBVTT)
            native_id = MXV_TRACK_TYPE_SUBTITLE;

        put_ebml_uint(pb, MXV_ID_TRACKTYPE, native_id);
        break;
    default:
        av_log(s, AV_LOG_ERROR, "Only audio, video, and subtitles are supported for MXV.\n");
        return AVERROR(EINVAL);
    }

    if (mxv->mode != MODE_WEBM || par->codec_id != AV_CODEC_ID_WEBVTT) {
        mxv->tracks[i].codecpriv_offset = avio_tell(pb);
        ret = mxv_write_codecprivate(s, pb, par, native_id, qt_id);
        if (ret < 0)
            return ret;
    }

    end_ebml_master(pb, track);

    return 0;
}

static int mxv_write_tracks(AVFormatContext *s)
{
    MXVMuxContext *mxv = s->priv_data;
    AVIOContext *pb = s->pb;
    int i, ret, default_stream_exists = 0;

    ret = mxv_add_seekhead_entry(mxv->seekhead, MXV_ID_TRACKS, avio_tell(pb));
    if (ret < 0)
        return ret;

    ret = start_ebml_master_crc32(pb, &mxv->tracks_bc, mxv, MXV_ID_TRACKS);
    if (ret < 0)
        return ret;

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        default_stream_exists |= st->disposition & AV_DISPOSITION_DEFAULT;
    }
    for (i = 0; i < s->nb_streams; i++) {
        ret = mxv_write_track(s, mxv, i, mxv->tracks_bc, default_stream_exists);
        if (ret < 0)
            return ret;
    }

    if ((pb->seekable & AVIO_SEEKABLE_NORMAL) && !mxv->is_live)
        end_ebml_master_crc32_preliminary(pb, &mxv->tracks_bc, mxv, &mxv->tracks_pos);
    else
        end_ebml_master_crc32(pb, &mxv->tracks_bc, mxv);

    return 0;
}

static int mxv_write_chapters(AVFormatContext *s)
{
    MXVMuxContext *mxv = s->priv_data;
    AVIOContext *dyn_cp, *pb = s->pb;
    ebml_master editionentry;
    AVRational scale = {1, 1E9};
    int i, ret;

    if (!s->nb_chapters || mxv->wrote_chapters)
        return 0;

    ret = mxv_add_seekhead_entry(mxv->seekhead, MXV_ID_CHAPTERS, avio_tell(pb));
    if (ret < 0) return ret;

    ret = start_ebml_master_crc32(pb, &dyn_cp, mxv, MXV_ID_CHAPTERS);
    if (ret < 0) return ret;

    editionentry = start_ebml_master(dyn_cp, MXV_ID_EDITIONENTRY, 0);
    if (mxv->mode != MODE_WEBM) {
        put_ebml_uint(dyn_cp, MXV_ID_EDITIONFLAGDEFAULT, 1);
        put_ebml_uint(dyn_cp, MXV_ID_EDITIONFLAGHIDDEN , 0);
    }
    for (i = 0; i < s->nb_chapters; i++) {
        ebml_master chapteratom, chapterdisplay;
        AVChapter *c     = s->chapters[i];
        int64_t chapterstart = av_rescale_q(c->start, c->time_base, scale);
        int64_t chapterend   = av_rescale_q(c->end,   c->time_base, scale);
        AVDictionaryEntry *t = NULL;
        if (chapterstart < 0 || chapterstart > chapterend || chapterend < 0) {
            av_log(s, AV_LOG_ERROR,
                   "Invalid chapter start (%"PRId64") or end (%"PRId64").\n",
                   chapterstart, chapterend);
            return AVERROR_INVALIDDATA;
        }

        chapteratom = start_ebml_master(dyn_cp, MXV_ID_CHAPTERATOM, 0);
        put_ebml_uint(dyn_cp, MXV_ID_CHAPTERUID, c->id + mxv->chapter_id_offset);
        put_ebml_uint(dyn_cp, MXV_ID_CHAPTERTIMESTART, chapterstart);
        put_ebml_uint(dyn_cp, MXV_ID_CHAPTERTIMEEND, chapterend);
        if (mxv->mode != MODE_WEBM) {
            put_ebml_uint(dyn_cp, MXV_ID_CHAPTERFLAGHIDDEN , 0);
            put_ebml_uint(dyn_cp, MXV_ID_CHAPTERFLAGENABLED, 1);
        }
        if ((t = av_dict_get(c->metadata, "title", NULL, 0))) {
            chapterdisplay = start_ebml_master(dyn_cp, MXV_ID_CHAPTERDISPLAY, 0);
            put_ebml_string(dyn_cp, MXV_ID_CHAPSTRING, t->value);
            put_ebml_string(dyn_cp, MXV_ID_CHAPLANG  , "und");
            end_ebml_master(dyn_cp, chapterdisplay);
        }
        end_ebml_master(dyn_cp, chapteratom);
    }
    end_ebml_master(dyn_cp, editionentry);
    end_ebml_master_crc32(pb, &dyn_cp, mxv);

    mxv->wrote_chapters = 1;
    return 0;
}

static int mxv_write_simpletag(AVIOContext *pb, AVDictionaryEntry *t)
{
    uint8_t *key = av_strdup(t->key);
    uint8_t *p   = key;
    const uint8_t *lang = NULL;
    ebml_master tag;

    if (!key)
        return AVERROR(ENOMEM);

    if ((p = strrchr(p, '-')) &&
        (lang = ff_convert_lang_to(p + 1, AV_LANG_ISO639_2_BIBL)))
        *p = 0;

    p = key;
    while (*p) {
        if (*p == ' ')
            *p = '_';
        else if (*p >= 'a' && *p <= 'z')
            *p -= 'a' - 'A';
        p++;
    }

    tag = start_ebml_master(pb, MXV_ID_SIMPLETAG, 0);
    put_ebml_string(pb, MXV_ID_TAGNAME, key);
    if (lang)
        put_ebml_string(pb, MXV_ID_TAGLANG, lang);
    put_ebml_string(pb, MXV_ID_TAGSTRING, t->value);
    end_ebml_master(pb, tag);

    av_freep(&key);
    return 0;
}

static int mxv_write_tag_targets(AVFormatContext *s, uint32_t elementid,
                                 unsigned int uid, ebml_master *tag)
{
    AVIOContext *pb;
    MXVMuxContext *mxv = s->priv_data;
    ebml_master targets;
    int ret;

    if (!mxv->tags_bc) {
        ret = mxv_add_seekhead_entry(mxv->seekhead, MXV_ID_TAGS, avio_tell(s->pb));
        if (ret < 0) return ret;

        start_ebml_master_crc32(s->pb, &mxv->tags_bc, mxv, MXV_ID_TAGS);
    }
    pb = mxv->tags_bc;

    *tag    = start_ebml_master(pb, MXV_ID_TAG,        0);
    targets = start_ebml_master(pb, MXV_ID_TAGTARGETS, 0);
    if (elementid)
        put_ebml_uint(pb, elementid, uid);
    end_ebml_master(pb, targets);
    return 0;
}

static int mxv_check_tag_name(const char *name, uint32_t elementid)
{
    return av_strcasecmp(name, "title") &&
           av_strcasecmp(name, "stereo_mode") &&
           av_strcasecmp(name, "creation_time") &&
           av_strcasecmp(name, "encoding_tool") &&
           av_strcasecmp(name, "duration") &&
           (elementid != MXV_ID_TAGTARGETS_TRACKUID ||
            av_strcasecmp(name, "language")) &&
           (elementid != MXV_ID_TAGTARGETS_ATTACHUID ||
            (av_strcasecmp(name, "filename") &&
             av_strcasecmp(name, "mimetype")));
}

static int mxv_write_tag(AVFormatContext *s, AVDictionary *m, uint32_t elementid,
                         unsigned int uid)
{
    MXVMuxContext *mxv = s->priv_data;
    ebml_master tag;
    int ret;
    AVDictionaryEntry *t = NULL;

    ret = mxv_write_tag_targets(s, elementid, uid, &tag);
    if (ret < 0)
        return ret;

    while ((t = av_dict_get(m, "", t, AV_DICT_IGNORE_SUFFIX))) {
        if (mxv_check_tag_name(t->key, elementid)) {
            ret = mxv_write_simpletag(mxv->tags_bc, t);
            if (ret < 0)
                return ret;
        }
    }

    end_ebml_master(mxv->tags_bc, tag);
    return 0;
}

static int mxv_check_tag(AVDictionary *m, uint32_t elementid)
{
    AVDictionaryEntry *t = NULL;

    while ((t = av_dict_get(m, "", t, AV_DICT_IGNORE_SUFFIX)))
        if (mxv_check_tag_name(t->key, elementid))
            return 1;

    return 0;
}

static int mxv_write_tags(AVFormatContext *s)
{
    MXVMuxContext *mxv = s->priv_data;
    int i, ret;

    ff_metadata_conv_ctx(s, ff_mxv_metadata_conv, NULL);

    if (mxv_check_tag(s->metadata, 0)) {
        ret = mxv_write_tag(s, s->metadata, 0, 0);
        if (ret < 0) return ret;
    }

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];

        if (st->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT)
            continue;

        if (!mxv_check_tag(st->metadata, MXV_ID_TAGTARGETS_TRACKUID))
            continue;

        ret = mxv_write_tag(s, st->metadata, MXV_ID_TAGTARGETS_TRACKUID, i + 1);
        if (ret < 0) return ret;
    }

    if ((s->pb->seekable & AVIO_SEEKABLE_NORMAL) && !mxv->is_live) {
        for (i = 0; i < s->nb_streams; i++) {
            AVIOContext *pb;
            AVStream *st = s->streams[i];
            ebml_master tag_target;
            ebml_master tag;

            if (st->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT)
                continue;

            mxv_write_tag_targets(s, MXV_ID_TAGTARGETS_TRACKUID, i + 1, &tag_target);
            pb = mxv->tags_bc;

            tag = start_ebml_master(pb, MXV_ID_SIMPLETAG, 0);
            put_ebml_string(pb, MXV_ID_TAGNAME, "DURATION");
            mxv->stream_duration_offsets[i] = avio_tell(pb);

            // Reserve space to write duration as a 20-byte string.
            // 2 (ebml id) + 1 (data size) + 20 (data)
            put_ebml_void(pb, 23);
            end_ebml_master(pb, tag);
            end_ebml_master(pb, tag_target);
        }
    }

    if (mxv->mode != MODE_WEBM) {
        for (i = 0; i < s->nb_chapters; i++) {
            AVChapter *ch = s->chapters[i];

            if (!mxv_check_tag(ch->metadata, MXV_ID_TAGTARGETS_CHAPTERUID))
                continue;

            ret = mxv_write_tag(s, ch->metadata, MXV_ID_TAGTARGETS_CHAPTERUID, ch->id + mxv->chapter_id_offset);
            if (ret < 0)
                return ret;
        }
    }

    if (mxv->have_attachments && mxv->mode != MODE_WEBM) {
        for (i = 0; i < mxv->attachments->num_entries; i++) {
            mxv_attachment *attachment = &mxv->attachments->entries[i];
            AVStream *st = s->streams[attachment->stream_idx];

            if (!mxv_check_tag(st->metadata, MXV_ID_TAGTARGETS_ATTACHUID))
                continue;

            ret = mxv_write_tag(s, st->metadata, MXV_ID_TAGTARGETS_ATTACHUID, attachment->fileuid);
            if (ret < 0)
                return ret;
        }
    }

    if (mxv->tags_bc) {
        if ((s->pb->seekable & AVIO_SEEKABLE_NORMAL) && !mxv->is_live)
            end_ebml_master_crc32_preliminary(s->pb, &mxv->tags_bc, mxv, &mxv->tags_pos);
        else
            end_ebml_master_crc32(s->pb, &mxv->tags_bc, mxv);
    }
    return 0;
}

static int mxv_write_attachments(AVFormatContext *s)
{
    MXVMuxContext *mxv = s->priv_data;
    AVIOContext *dyn_cp, *pb = s->pb;
    AVLFG c;
    int i, ret;

    if (!mxv->have_attachments)
        return 0;

    mxv->attachments = av_mallocz(sizeof(*mxv->attachments));
    if (!mxv->attachments)
        return AVERROR(ENOMEM);

    av_lfg_init(&c, av_get_random_seed());

    ret = mxv_add_seekhead_entry(mxv->seekhead, MXV_ID_ATTACHMENTS, avio_tell(pb));
    if (ret < 0) return ret;

    ret = start_ebml_master_crc32(pb, &dyn_cp, mxv, MXV_ID_ATTACHMENTS);
    if (ret < 0) return ret;

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        ebml_master attached_file;
        mxv_attachment *attachment = mxv->attachments->entries;
        AVDictionaryEntry *t;
        const char *mimetype = NULL;
        uint32_t fileuid;

        if (st->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT)
            continue;

        attachment = av_realloc_array(attachment, mxv->attachments->num_entries + 1, sizeof(mxv_attachment));
        if (!attachment)
            return AVERROR(ENOMEM);
        mxv->attachments->entries = attachment;

        attached_file = start_ebml_master(dyn_cp, MXV_ID_ATTACHEDFILE, 0);

        if (t = av_dict_get(st->metadata, "title", NULL, 0))
            put_ebml_string(dyn_cp, MXV_ID_FILEDESC, t->value);
        if (!(t = av_dict_get(st->metadata, "filename", NULL, 0))) {
            av_log(s, AV_LOG_ERROR, "Attachment stream %d has no filename tag.\n", i);
            return AVERROR(EINVAL);
        }
        put_ebml_string(dyn_cp, MXV_ID_FILENAME, t->value);
        if (t = av_dict_get(st->metadata, "mimetype", NULL, 0))
            mimetype = t->value;
        else if (st->codecpar->codec_id != AV_CODEC_ID_NONE ) {
            int i;
            for (i = 0; ff_mxv_mime_tags[i].id != AV_CODEC_ID_NONE; i++)
                if (ff_mxv_mime_tags[i].id == st->codecpar->codec_id) {
                    mimetype = ff_mxv_mime_tags[i].str;
                    break;
                }
            for (i = 0; ff_mxv_image_mime_tags[i].id != AV_CODEC_ID_NONE; i++)
                if (ff_mxv_image_mime_tags[i].id == st->codecpar->codec_id) {
                    mimetype = ff_mxv_image_mime_tags[i].str;
                    break;
                }
        }
        if (!mimetype) {
            av_log(s, AV_LOG_ERROR, "Attachment stream %d has no mimetype tag and "
                                    "it cannot be deduced from the codec id.\n", i);
            return AVERROR(EINVAL);
        }

        if (s->flags & AVFMT_FLAG_BITEXACT) {
            struct AVSHA *sha = av_sha_alloc();
            uint8_t digest[20];
            if (!sha)
                return AVERROR(ENOMEM);
            av_sha_init(sha, 160);
            av_sha_update(sha, st->codecpar->extradata, st->codecpar->extradata_size);
            av_sha_final(sha, digest);
            av_free(sha);
            fileuid = AV_RL32(digest);
        } else {
            fileuid = av_lfg_get(&c);
        }
        av_log(s, AV_LOG_VERBOSE, "Using %.8"PRIx32" for attachment %d\n",
               fileuid, mxv->attachments->num_entries);

        put_ebml_string(dyn_cp, MXV_ID_FILEMIMETYPE, mimetype);
        put_ebml_binary(dyn_cp, MXV_ID_FILEDATA, st->codecpar->extradata, st->codecpar->extradata_size);
        put_ebml_uint(dyn_cp, MXV_ID_FILEUID, fileuid);
        end_ebml_master(dyn_cp, attached_file);

        mxv->attachments->entries[mxv->attachments->num_entries].stream_idx = i;
        mxv->attachments->entries[mxv->attachments->num_entries++].fileuid  = fileuid;
    }
    end_ebml_master_crc32(pb, &dyn_cp, mxv);

    return 0;
}

static int64_t get_metadata_duration(AVFormatContext *s)
{
    int i = 0;
    int64_t max = 0;
    int64_t us;

    AVDictionaryEntry *explicitDuration = av_dict_get(s->metadata, "DURATION", NULL, 0);
    if (explicitDuration && (av_parse_time(&us, explicitDuration->value, 1) == 0) && us > 0) {
        av_log(s, AV_LOG_DEBUG, "get_metadata_duration found duration in context metadata: %" PRId64 "\n", us);
        return us;
    }

    for (i = 0; i < s->nb_streams; i++) {
        int64_t us;
        AVDictionaryEntry *duration = av_dict_get(s->streams[i]->metadata, "DURATION", NULL, 0);

        if (duration && (av_parse_time(&us, duration->value, 1) == 0))
            max = FFMAX(max, us);
    }

    av_log(s, AV_LOG_DEBUG, "get_metadata_duration returned: %" PRId64 "\n", max);
    return max;
}

static int mxv_write_header(AVFormatContext *s)
{
    MXVMuxContext *mxv = s->priv_data;
    AVIOContext *pb = s->pb;
    ebml_master ebml_header;
    AVDictionaryEntry *tag;
    int ret, i, version = 2;
    int64_t creation_time;

    if (!strcmp(s->oformat->name, "webm")) {
        mxv->mode      = MODE_WEBM;
        mxv->write_crc = 0;
    } else
        mxv->mode = MODE_MXVv2;

    if (mxv->mode != MODE_WEBM ||
        av_dict_get(s->metadata, "stereo_mode", NULL, 0) ||
        av_dict_get(s->metadata, "alpha_mode", NULL, 0))
        version = 4;

    for (i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->codecpar->codec_id == AV_CODEC_ID_OPUS ||
            av_dict_get(s->streams[i]->metadata, "stereo_mode", NULL, 0) ||
            av_dict_get(s->streams[i]->metadata, "alpha_mode", NULL, 0))
            version = 4;
    }

    mxv->tracks = av_mallocz_array(s->nb_streams, sizeof(*mxv->tracks));
    if (!mxv->tracks) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ebml_header = start_ebml_master(pb, EBML_ID_HEADER, MAX_EBML_HEADER_SIZE);
    put_ebml_uint  (pb, EBML_ID_EBMLVERSION       ,           1);
    put_ebml_uint  (pb, EBML_ID_EBMLREADVERSION   ,           1);
    put_ebml_uint  (pb, EBML_ID_EBMLMAXIDLENGTH   ,           4);
    put_ebml_uint  (pb, EBML_ID_EBMLMAXSIZELENGTH ,           8);
    put_ebml_string(pb, EBML_ID_DOCTYPE           , s->oformat->name);
    put_ebml_uint  (pb, EBML_ID_DOCTYPEVERSION    ,     version);
    put_ebml_uint  (pb, EBML_ID_DOCTYPEREADVERSION,           2);
    end_ebml_master(pb, ebml_header);

    mxv->segment = start_ebml_master(pb, MXV_ID_SEGMENT, 0);
    mxv->segment_offset = avio_tell(pb);

    // we write a seek head at the beginning to point to all other level
    // one elements, which aren't more than 10 elements as we write only one
    // of every other currently defined level 1 element
    mxv->seekhead = mxv_start_seekhead(pb, mxv->segment_offset, 10);
    if (!mxv->seekhead) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = mxv_add_seekhead_entry(mxv->seekhead, MXV_ID_INFO, avio_tell(pb));
    if (ret < 0) goto fail;

    ret = start_ebml_master_crc32(pb, &mxv->info_bc, mxv, MXV_ID_INFO);
    if (ret < 0)
        return ret;
    pb = mxv->info_bc;

    put_ebml_uint(pb, MXV_ID_TIMECODESCALE, 1000000);
    if ((tag = av_dict_get(s->metadata, "title", NULL, 0)))
        put_ebml_string(pb, MXV_ID_TITLE, tag->value);
    if (!(s->flags & AVFMT_FLAG_BITEXACT)) {
        put_ebml_string(pb, MXV_ID_MUXINGAPP, LIBAVFORMAT_IDENT);
        if ((tag = av_dict_get(s->metadata, "encoding_tool", NULL, 0)))
            put_ebml_string(pb, MXV_ID_WRITINGAPP, tag->value);
        else
            put_ebml_string(pb, MXV_ID_WRITINGAPP, LIBAVFORMAT_IDENT);

        if (mxv->mode != MODE_WEBM) {
            uint32_t segment_uid[4];
            AVLFG lfg;

            av_lfg_init(&lfg, av_get_random_seed());

            for (i = 0; i < 4; i++)
                segment_uid[i] = av_lfg_get(&lfg);

            put_ebml_binary(pb, MXV_ID_SEGMENTUID, segment_uid, 16);
        }
    } else {
        const char *ident = "Lavf";
        put_ebml_string(pb, MXV_ID_MUXINGAPP , ident);
        put_ebml_string(pb, MXV_ID_WRITINGAPP, ident);
    }

    if (ff_parse_creation_time_metadata(s, &creation_time, 0) > 0) {
        // Adjust time so it's relative to 2001-01-01 and convert to nanoseconds.
        int64_t date_utc = (creation_time - 978307200000000LL) * 1000;
        uint8_t date_utc_buf[8];
        AV_WB64(date_utc_buf, date_utc);
        put_ebml_binary(pb, MXV_ID_DATEUTC, date_utc_buf, 8);
    }

    // reserve space for the duration
    mxv->duration = 0;
    mxv->duration_offset = avio_tell(pb);
    if (!mxv->is_live) {
        int64_t metadata_duration = get_metadata_duration(s);

        if (s->duration > 0) {
            int64_t scaledDuration = av_rescale(s->duration, 1000, AV_TIME_BASE);
            put_ebml_float(pb, MXV_ID_DURATION, scaledDuration);
            av_log(s, AV_LOG_DEBUG, "Write early duration from recording time = %" PRIu64 "\n", scaledDuration);
        } else if (metadata_duration > 0) {
            int64_t scaledDuration = av_rescale(metadata_duration, 1000, AV_TIME_BASE);
            put_ebml_float(pb, MXV_ID_DURATION, scaledDuration);
            av_log(s, AV_LOG_DEBUG, "Write early duration from metadata = %" PRIu64 "\n", scaledDuration);
        } else {
            put_ebml_void(pb, 11);              // assumes double-precision float to be written
        }
    }
    if ((s->pb->seekable & AVIO_SEEKABLE_NORMAL) && !mxv->is_live)
        end_ebml_master_crc32_preliminary(s->pb, &mxv->info_bc, mxv, &mxv->info_pos);
    else
        end_ebml_master_crc32(s->pb, &mxv->info_bc, mxv);
    pb = s->pb;

    // initialize stream_duration fields
    mxv->stream_durations        = av_mallocz(s->nb_streams * sizeof(int64_t));
    mxv->stream_duration_offsets = av_mallocz(s->nb_streams * sizeof(int64_t));
    if (!mxv->stream_durations || !mxv->stream_duration_offsets) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    mxv->aes_key = av_malloc(TRACK_ENCRYPTION_KEY_SIZE);
    ff_mxv_generate_aes_key(mxv->aes_key, TRACK_ENCRYPTION_KEY_SIZE);
    ret = mxv_write_tracks(s);
    if (ret < 0)
        goto fail;

    for (i = 0; i < s->nb_chapters; i++)
        mxv->chapter_id_offset = FFMAX(mxv->chapter_id_offset, 1LL - s->chapters[i]->id);

    ret = mxv_write_chapters(s);
    if (ret < 0)
        goto fail;

    if (mxv->mode != MODE_WEBM) {
        ret = mxv_write_attachments(s);
        if (ret < 0)
            goto fail;
    }

    ret = mxv_write_tags(s);
    if (ret < 0)
        goto fail;

    if (!(s->pb->seekable & AVIO_SEEKABLE_NORMAL) && !mxv->is_live)
        mxv_write_seekhead(pb, mxv);

    mxv->cues = mxv_start_cues(mxv->segment_offset);
    if (!mxv->cues) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if (s->metadata_header_padding > 0) {
        if (s->metadata_header_padding == 1)
            s->metadata_header_padding++;
        put_ebml_void(pb, s->metadata_header_padding);
    }

    if ((pb->seekable & AVIO_SEEKABLE_NORMAL) && mxv->reserve_cues_space) {
        mxv->cues_pos = avio_tell(pb);
        if (mxv->reserve_cues_space == 1)
            mxv->reserve_cues_space++;
        put_ebml_void(pb, mxv->reserve_cues_space);
    }

    av_init_packet(&mxv->cur_audio_pkt);
    mxv->cur_audio_pkt.size = 0;
    mxv->cluster_pos = -1;

    avio_flush(pb);

    // start a new cluster every 5 MB or 5 sec, or 32k / 1 sec for streaming or
    // after 4k and on a keyframe
    if (pb->seekable & AVIO_SEEKABLE_NORMAL) {
        if (mxv->cluster_time_limit < 0)
            mxv->cluster_time_limit = 5000;
        if (mxv->cluster_size_limit < 0)
            mxv->cluster_size_limit = 5 * 1024 * 1024;
    } else {
        if (mxv->cluster_time_limit < 0)
            mxv->cluster_time_limit = 1000;
        if (mxv->cluster_size_limit < 0)
            mxv->cluster_size_limit = 32 * 1024;
    }

    return 0;
fail:
    mxv_free(mxv);
    return ret;
}

static int mxv_blockgroup_size(int pkt_size)
{
    int size = pkt_size + 4;
    size += ebml_num_size(size);
    size += 2;              // EBML ID for block and block duration
    size += 9;              // max size of block duration incl. length field
    return size;
}

static int mxv_strip_wavpack(const uint8_t *src, uint8_t **pdst, int *size)
{
    uint8_t *dst;
    int srclen = *size;
    int offset = 0;
    int ret;

    dst = av_malloc(srclen);
    if (!dst)
        return AVERROR(ENOMEM);

    while (srclen >= WV_HEADER_SIZE) {
        WvHeader header;

        ret = ff_wv_parse_header(&header, src);
        if (ret < 0)
            goto fail;
        src    += WV_HEADER_SIZE;
        srclen -= WV_HEADER_SIZE;

        if (srclen < header.blocksize) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        if (header.initial) {
            AV_WL32(dst + offset, header.samples);
            offset += 4;
        }
        AV_WL32(dst + offset,     header.flags);
        AV_WL32(dst + offset + 4, header.crc);
        offset += 8;

        if (!(header.initial && header.final)) {
            AV_WL32(dst + offset, header.blocksize);
            offset += 4;
        }

        memcpy(dst + offset, src, header.blocksize);
        src    += header.blocksize;
        srclen -= header.blocksize;
        offset += header.blocksize;
    }

    *pdst = dst;
    *size = offset;

    return 0;
fail:
    av_freep(&dst);
    return ret;
}

static void mxv_write_block(AVFormatContext *s, AVIOContext *pb,
                            uint32_t blockid, AVPacket *pkt, int keyframe)
{
    MXVMuxContext *mxv = s->priv_data;
    AVCodecParameters *par = s->streams[pkt->stream_index]->codecpar;
    uint8_t *data = NULL, *side_data = NULL;
    int offset = 0, size = pkt->size, side_data_size = 0;
    int64_t ts = mxv->tracks[pkt->stream_index].write_dts ? pkt->dts : pkt->pts;
    uint64_t additional_id = 0;
    int64_t discard_padding = 0;
    uint8_t track_number = (mxv->is_dash ? mxv->dash_track_number : (pkt->stream_index + 1));
    ebml_master block_group, block_additions, block_more;

    ts += mxv->tracks[pkt->stream_index].ts_offset;

    /* The following string is identical to the one in mxv_write_vtt_blocks
     * so that only one copy needs to exist in binaries. */
    av_log(s, AV_LOG_DEBUG,
           "Writing block of size %d with pts %" PRId64 ", dts %" PRId64 ", "
           "duration %" PRId64 " at relative offset %" PRId64 " in cluster "
           "at offset %" PRId64 ". TrackNumber %d, keyframe %d\n",
           pkt->size, pkt->pts, pkt->dts, pkt->duration, avio_tell(pb),
           mxv->cluster_pos, track_number, keyframe != 0);
    if (par->codec_id == AV_CODEC_ID_H264 && par->extradata_size > 0 &&
        (AV_RB24(par->extradata) == 1 || AV_RB32(par->extradata) == 1))
        ff_avc_parse_nal_units_buf(pkt->data, &data, &size);
    else if (par->codec_id == AV_CODEC_ID_HEVC && par->extradata_size > 6 &&
             (AV_RB24(par->extradata) == 1 || AV_RB32(par->extradata) == 1))
        /* extradata is Annex B, assume the bitstream is too and convert it */
        ff_hevc_annexb2mp4_buf(pkt->data, &data, &size, 0, NULL);
    else if (par->codec_id == AV_CODEC_ID_AV1)
        ff_av1_filter_obus_buf(pkt->data, &data, &size, &offset);
    else if (par->codec_id == AV_CODEC_ID_WAVPACK) {
        int ret = mxv_strip_wavpack(pkt->data, &data, &size);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Error stripping a WavPack packet.\n");
            return;
        }
    } else
        data = pkt->data;

    if (par->codec_id == AV_CODEC_ID_PRORES && size >= 8) {
        /* MXV specification requires to remove the first QuickTime atom
         */
        size  -= 8;
        offset = 8;
    }

    side_data = av_packet_get_side_data(pkt,
                                        AV_PKT_DATA_SKIP_SAMPLES,
                                        &side_data_size);

    if (side_data && side_data_size >= 10) {
        discard_padding = av_rescale_q(AV_RL32(side_data + 4),
                                       (AVRational){1, par->sample_rate},
                                       (AVRational){1, 1000000000});
    }

    side_data = av_packet_get_side_data(pkt,
                                        AV_PKT_DATA_MXV_BLOCKADDITIONAL,
                                        &side_data_size);
    if (side_data) {
        additional_id = AV_RB64(side_data);
        side_data += 8;
        side_data_size -= 8;
    }

    if ((side_data_size && additional_id == 1) || discard_padding) {
        block_group = start_ebml_master(pb, MXV_ID_BLOCKGROUP, 0);
        blockid = MXV_ID_BLOCK;
    }

    uint8_t *writeData = av_mallocz(size);
    ff_mxv_encrypt_aes128(writeData, mxv->aes_key, data, size);

    put_ebml_id(pb, blockid);
    put_ebml_num(pb, size + 4, 0);
    // this assumes stream_index is less than 126
    avio_w8(pb, 0x80 | track_number);
    avio_wb16(pb, ts - mxv->cluster_pts);
    avio_w8(pb, (blockid == MXV_ID_SIMPLEBLOCK && keyframe) ? (1 << 7) : 0);
    avio_write(pb, writeData + offset, size);
    if (data != pkt->data)
        av_free(data);
    av_free(writeData);

    if (blockid == MXV_ID_BLOCK && !keyframe) {
        put_ebml_sint(pb, MXV_ID_BLOCKREFERENCE,
                      mxv->last_track_timestamp[track_number - 1]);
    }
    mxv->last_track_timestamp[track_number - 1] = ts - mxv->cluster_pts;

    if (discard_padding) {
        put_ebml_sint(pb, MXV_ID_DISCARDPADDING, discard_padding);
    }

    if (side_data_size && additional_id == 1) {
        block_additions = start_ebml_master(pb, MXV_ID_BLOCKADDITIONS, 0);
        block_more = start_ebml_master(pb, MXV_ID_BLOCKMORE, 0);
        put_ebml_uint(pb, MXV_ID_BLOCKADDID, 1);
        put_ebml_id(pb, MXV_ID_BLOCKADDITIONAL);
        put_ebml_num(pb, side_data_size, 0);
        avio_write(pb, side_data, side_data_size);
        end_ebml_master(pb, block_more);
        end_ebml_master(pb, block_additions);
    }
    if ((side_data_size && additional_id == 1) || discard_padding) {
        end_ebml_master(pb, block_group);
    }
}

static int mxv_write_vtt_blocks(AVFormatContext *s, AVIOContext *pb, AVPacket *pkt)
{
    MXVMuxContext *mxv = s->priv_data;
    ebml_master blockgroup;
    int id_size, settings_size, size;
    uint8_t *id, *settings;
    int64_t ts = mxv->tracks[pkt->stream_index].write_dts ? pkt->dts : pkt->pts;
    const int flags = 0;

    id_size = 0;
    id = av_packet_get_side_data(pkt, AV_PKT_DATA_WEBVTT_IDENTIFIER,
                                 &id_size);

    settings_size = 0;
    settings = av_packet_get_side_data(pkt, AV_PKT_DATA_WEBVTT_SETTINGS,
                                       &settings_size);

    size = id_size + 1 + settings_size + 1 + pkt->size;

    /* The following string is identical to the one in mxv_write_block so that
     * only one copy needs to exist in binaries. */
    av_log(s, AV_LOG_DEBUG,
           "Writing block of size %d with pts %" PRId64 ", dts %" PRId64 ", "
           "duration %" PRId64 " at relative offset %" PRId64 " in cluster "
           "at offset %" PRId64 ". TrackNumber %d, keyframe %d\n",
           size, pkt->pts, pkt->dts, pkt->duration, avio_tell(pb),
           mxv->cluster_pos, pkt->stream_index + 1, 1);

    blockgroup = start_ebml_master(pb, MXV_ID_BLOCKGROUP, mxv_blockgroup_size(size));

    put_ebml_id(pb, MXV_ID_BLOCK);
    put_ebml_num(pb, size + 4, 0);
    avio_w8(pb, 0x80 | (pkt->stream_index + 1));     // this assumes stream_index is less than 126
    avio_wb16(pb, ts - mxv->cluster_pts);
    avio_w8(pb, flags);
    avio_printf(pb, "%.*s\n%.*s\n%.*s", id_size, id, settings_size, settings, pkt->size, pkt->data);

    put_ebml_uint(pb, MXV_ID_BLOCKDURATION, pkt->duration);
    end_ebml_master(pb, blockgroup);

    return pkt->duration;
}

static void mxv_start_new_cluster(AVFormatContext *s, AVPacket *pkt)
{
    MXVMuxContext *mxv = s->priv_data;

    end_ebml_master_crc32(s->pb, &mxv->cluster_bc, mxv);
    mxv->cluster_pos = -1;
    av_log(s, AV_LOG_DEBUG,
           "Starting new cluster at offset %" PRIu64 " bytes, "
           "pts %" PRIu64 ", dts %" PRIu64 "\n",
           avio_tell(s->pb), pkt->pts, pkt->dts);
    avio_flush(s->pb);
}

static int mxv_check_new_extra_data(AVFormatContext *s, AVPacket *pkt)
{
    MXVMuxContext *mxv = s->priv_data;
    mxv_track *track        = &mxv->tracks[pkt->stream_index];
    AVCodecParameters *par  = s->streams[pkt->stream_index]->codecpar;
    uint8_t *side_data;
    int side_data_size = 0, ret;

    side_data = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA,
                                        &side_data_size);

    switch (par->codec_id) {
    case AV_CODEC_ID_AAC:
        if (side_data_size && (s->pb->seekable & AVIO_SEEKABLE_NORMAL) && !mxv->is_live) {
            int filler, output_sample_rate = 0;
            int64_t curpos;
            ret = get_aac_sample_rates(s, side_data, side_data_size, &track->sample_rate,
                                       &output_sample_rate);
            if (ret < 0)
                return ret;
            if (!output_sample_rate)
                output_sample_rate = track->sample_rate; // Space is already reserved, so it's this or a void element.
            av_freep(&par->extradata);
            ret = ff_alloc_extradata(par, side_data_size);
            if (ret < 0)
                return ret;
            memcpy(par->extradata, side_data, side_data_size);
            curpos = avio_tell(mxv->tracks_bc);
            avio_seek(mxv->tracks_bc, track->codecpriv_offset, SEEK_SET);
            mxv_write_codecprivate(s, mxv->tracks_bc, par, 1, 0);
            filler = MAX_PCE_SIZE + 2 + 4 - (avio_tell(mxv->tracks_bc) - track->codecpriv_offset);
            if (filler)
                put_ebml_void(mxv->tracks_bc, filler);
            avio_seek(mxv->tracks_bc, track->sample_rate_offset, SEEK_SET);
            put_ebml_float(mxv->tracks_bc, MXV_ID_AUDIOSAMPLINGFREQ, track->sample_rate);
            put_ebml_float(mxv->tracks_bc, MXV_ID_AUDIOOUTSAMPLINGFREQ, output_sample_rate);
            avio_seek(mxv->tracks_bc, curpos, SEEK_SET);
        } else if (!par->extradata_size && !track->sample_rate) {
            // No extradata (codecpar or packet side data).
            av_log(s, AV_LOG_ERROR, "Error parsing AAC extradata, unable to determine samplerate.\n");
            return AVERROR(EINVAL);
        }
        break;
    case AV_CODEC_ID_FLAC:
        if (side_data_size && (s->pb->seekable & AVIO_SEEKABLE_NORMAL) && !mxv->is_live) {
            AVCodecParameters *codecpriv_par;
            int64_t curpos;
            if (side_data_size != par->extradata_size) {
                av_log(s, AV_LOG_ERROR, "Invalid FLAC STREAMINFO metadata for output stream %d\n",
                       pkt->stream_index);
                return AVERROR(EINVAL);
            }
            codecpriv_par = avcodec_parameters_alloc();
            if (!codecpriv_par)
                return AVERROR(ENOMEM);
            ret = avcodec_parameters_copy(codecpriv_par, par);
            if (ret < 0) {
                avcodec_parameters_free(&codecpriv_par);
                return ret;
            }
            memcpy(codecpriv_par->extradata, side_data, side_data_size);
            curpos = avio_tell(mxv->tracks_bc);
            avio_seek(mxv->tracks_bc, track->codecpriv_offset, SEEK_SET);
            mxv_write_codecprivate(s, mxv->tracks_bc, codecpriv_par, 1, 0);
            avio_seek(mxv->tracks_bc, curpos, SEEK_SET);
            avcodec_parameters_free(&codecpriv_par);
        }
        break;
    // FIXME: Remove the following once libaom starts propagating extradata during init()
    //        See https://bugs.chromium.org/p/aomedia/issues/detail?id=2012
    case AV_CODEC_ID_AV1:
        if (side_data_size && (s->pb->seekable & AVIO_SEEKABLE_NORMAL) && !mxv->is_live &&
            !par->extradata_size) {
            AVIOContext *dyn_cp;
            uint8_t *codecpriv;
            int codecpriv_size;
            int64_t curpos;
            ret = avio_open_dyn_buf(&dyn_cp);
            if (ret < 0)
                return ret;
            ff_isom_write_av1c(dyn_cp, side_data, side_data_size);
            codecpriv_size = avio_close_dyn_buf(dyn_cp, &codecpriv);
            if (!codecpriv_size) {
                av_free(codecpriv);
                return AVERROR_INVALIDDATA;
            }
            curpos = avio_tell(mxv->tracks_bc);
            avio_seek(mxv->tracks_bc, track->codecpriv_offset, SEEK_SET);
            // Do not write the OBUs as we don't have space saved for them
            put_ebml_binary(mxv->tracks_bc, MXV_ID_CODECPRIVATE, codecpriv, 4);
            av_free(codecpriv);
            avio_seek(mxv->tracks_bc, curpos, SEEK_SET);
            ret = ff_alloc_extradata(par, side_data_size);
            if (ret < 0)
                return ret;
            memcpy(par->extradata, side_data, side_data_size);
        } else if (!par->extradata_size)
            return AVERROR_INVALIDDATA;
        break;
    default:
        if (side_data_size)
            av_log(s, AV_LOG_DEBUG, "Ignoring new extradata in a packet for stream %d.\n", pkt->stream_index);
        break;
    }

    return 0;
}

static int mxv_write_packet_internal(AVFormatContext *s, AVPacket *pkt, int add_cue)
{
    MXVMuxContext *mxv = s->priv_data;
    AVIOContext *pb         = s->pb;
    AVCodecParameters *par  = s->streams[pkt->stream_index]->codecpar;
    int keyframe            = !!(pkt->flags & AV_PKT_FLAG_KEY);
    int duration            = pkt->duration;
    int ret;
    int64_t ts = mxv->tracks[pkt->stream_index].write_dts ? pkt->dts : pkt->pts;
    int64_t relative_packet_pos;
    int dash_tracknum = mxv->is_dash ? mxv->dash_track_number : pkt->stream_index + 1;

    if (ts == AV_NOPTS_VALUE) {
        av_log(s, AV_LOG_ERROR, "Can't write packet with unknown timestamp\n");
        return AVERROR(EINVAL);
    }
    ts += mxv->tracks[pkt->stream_index].ts_offset;

    if (mxv->cluster_pos != -1) {
        int64_t cluster_time = ts - mxv->cluster_pts;
        if ((int16_t)cluster_time != cluster_time) {
            av_log(s, AV_LOG_WARNING, "Starting new cluster due to timestamp\n");
            mxv_start_new_cluster(s, pkt);
        }
    }

    if (mxv->cluster_pos == -1) {
        mxv->cluster_pos = avio_tell(s->pb);
        ret = start_ebml_master_crc32(s->pb, &mxv->cluster_bc, mxv, MXV_ID_CLUSTER);
        if (ret < 0)
            return ret;
        put_ebml_uint(mxv->cluster_bc, MXV_ID_CLUSTERTIMECODE, FFMAX(0, ts));
        mxv->cluster_pts = FFMAX(0, ts);
    }
    pb = mxv->cluster_bc;

    relative_packet_pos = avio_tell(pb);

    if (par->codec_type != AVMEDIA_TYPE_SUBTITLE) {
        mxv_write_block(s, pb, MXV_ID_SIMPLEBLOCK, pkt, keyframe);
        if ((s->pb->seekable & AVIO_SEEKABLE_NORMAL) && (par->codec_type == AVMEDIA_TYPE_VIDEO && keyframe || add_cue)) {
            ret = mxv_add_cuepoint(mxv->cues, pkt->stream_index, dash_tracknum, ts, mxv->cluster_pos, relative_packet_pos, -1);
            if (ret < 0) return ret;
        }
    } else {
        if (par->codec_id == AV_CODEC_ID_WEBVTT) {
            duration = mxv_write_vtt_blocks(s, pb, pkt);
        } else {
            ebml_master blockgroup = start_ebml_master(pb, MXV_ID_BLOCKGROUP,
                                                       mxv_blockgroup_size(pkt->size));

#if FF_API_CONVERGENCE_DURATION
FF_DISABLE_DEPRECATION_WARNINGS
            /* For backward compatibility, prefer convergence_duration. */
            if (pkt->convergence_duration > 0) {
                duration = pkt->convergence_duration;
            }
FF_ENABLE_DEPRECATION_WARNINGS
#endif
            /* All subtitle blocks are considered to be keyframes. */
            mxv_write_block(s, pb, MXV_ID_BLOCK, pkt, 1);
            put_ebml_uint(pb, MXV_ID_BLOCKDURATION, duration);
            end_ebml_master(pb, blockgroup);
        }

        if (s->pb->seekable & AVIO_SEEKABLE_NORMAL) {
            ret = mxv_add_cuepoint(mxv->cues, pkt->stream_index, dash_tracknum, ts,
                                   mxv->cluster_pos, relative_packet_pos, duration);
            if (ret < 0)
                return ret;
        }
    }

    mxv->duration = FFMAX(mxv->duration, ts + duration);

    if (mxv->stream_durations)
        mxv->stream_durations[pkt->stream_index] =
            FFMAX(mxv->stream_durations[pkt->stream_index], ts + duration);

    return 0;
}

static int mxv_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    MXVMuxContext *mxv = s->priv_data;
    int codec_type          = s->streams[pkt->stream_index]->codecpar->codec_type;
    int keyframe            = !!(pkt->flags & AV_PKT_FLAG_KEY);
    int cluster_size;
    int64_t cluster_time;
    int ret;
    int start_new_cluster;

    ret = mxv_check_new_extra_data(s, pkt);
    if (ret < 0)
        return ret;

    if (mxv->tracks[pkt->stream_index].write_dts)
        cluster_time = pkt->dts - mxv->cluster_pts;
    else
        cluster_time = pkt->pts - mxv->cluster_pts;
    cluster_time += mxv->tracks[pkt->stream_index].ts_offset;

    // start a new cluster every 5 MB or 5 sec, or 32k / 1 sec for streaming or
    // after 4k and on a keyframe
    cluster_size = avio_tell(mxv->cluster_bc);

    if (mxv->is_dash && codec_type == AVMEDIA_TYPE_VIDEO) {
        // WebM DASH specification states that the first block of every cluster
        // has to be a key frame. So for DASH video, we only create a cluster
        // on seeing key frames.
        start_new_cluster = keyframe;
    } else if (mxv->is_dash && codec_type == AVMEDIA_TYPE_AUDIO &&
               (mxv->cluster_pos == -1 ||
                cluster_time > mxv->cluster_time_limit)) {
        // For DASH audio, we create a Cluster based on cluster_time_limit
        start_new_cluster = 1;
    } else if (!mxv->is_dash &&
               (cluster_size > mxv->cluster_size_limit ||
                cluster_time > mxv->cluster_time_limit ||
                (codec_type == AVMEDIA_TYPE_VIDEO && keyframe &&
                 cluster_size > 4 * 1024))) {
        start_new_cluster = 1;
    } else {
        start_new_cluster = 0;
    }

    if (mxv->cluster_pos != -1 && start_new_cluster) {
        mxv_start_new_cluster(s, pkt);
    }

    if (!mxv->cluster_pos)
        avio_write_marker(s->pb,
                          av_rescale_q(pkt->dts, s->streams[pkt->stream_index]->time_base, AV_TIME_BASE_Q),
                          keyframe && (mxv->have_video ? codec_type == AVMEDIA_TYPE_VIDEO : 1) ? AVIO_DATA_MARKER_SYNC_POINT : AVIO_DATA_MARKER_BOUNDARY_POINT);

    // check if we have an audio packet cached
    if (mxv->cur_audio_pkt.size > 0) {
        // for DASH audio, a CuePoint has to be added when there is a new cluster.
        ret = mxv_write_packet_internal(s, &mxv->cur_audio_pkt,
                                        mxv->is_dash ? start_new_cluster : 0);
        av_packet_unref(&mxv->cur_audio_pkt);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR,
                   "Could not write cached audio packet ret:%d\n", ret);
            return ret;
        }
    }

    // buffer an audio packet to ensure the packet containing the video
    // keyframe's timecode is contained in the same cluster for WebM
    if (codec_type == AVMEDIA_TYPE_AUDIO) {
        if (pkt->size > 0)
            ret = av_packet_ref(&mxv->cur_audio_pkt, pkt);
    } else
        ret = mxv_write_packet_internal(s, pkt, 0);
    return ret;
}

static int mxv_write_flush_packet(AVFormatContext *s, AVPacket *pkt)
{
    MXVMuxContext *mxv = s->priv_data;

    if (!pkt) {
        if (mxv->cluster_pos != -1) {
            end_ebml_master_crc32(s->pb, &mxv->cluster_bc, mxv);
            mxv->cluster_pos = -1;
            av_log(s, AV_LOG_DEBUG,
                   "Flushing cluster at offset %" PRIu64 " bytes\n",
                   avio_tell(s->pb));
            avio_flush(s->pb);
        }
        return 1;
    }
    return mxv_write_packet(s, pkt);
}

static int mxv_write_trailer(AVFormatContext *s)
{
    MXVMuxContext *mxv = s->priv_data;
    AVIOContext *pb = s->pb;
    int64_t currentpos, cuespos;
    int ret;

    // check if we have an audio packet cached
    if (mxv->cur_audio_pkt.size > 0) {
        ret = mxv_write_packet_internal(s, &mxv->cur_audio_pkt, 0);
        av_packet_unref(&mxv->cur_audio_pkt);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR,
                   "Could not write cached audio packet ret:%d\n", ret);
            return ret;
        }
    }

    if (mxv->cluster_bc) {
        end_ebml_master_crc32(pb, &mxv->cluster_bc, mxv);
    }

    ret = mxv_write_chapters(s);
    if (ret < 0)
        return ret;


    if ((pb->seekable & AVIO_SEEKABLE_NORMAL) && !mxv->is_live) {
        if (mxv->cues->num_entries) {
            if (mxv->reserve_cues_space) {
                int64_t cues_end;

                currentpos = avio_tell(pb);
                avio_seek(pb, mxv->cues_pos, SEEK_SET);

                cuespos  = mxv_write_cues(s, mxv->cues, mxv->tracks, s->nb_streams);
                cues_end = avio_tell(pb);
                if (cues_end > cuespos + mxv->reserve_cues_space) {
                    av_log(s, AV_LOG_ERROR,
                           "Insufficient space reserved for cues: %d "
                           "(needed: %" PRId64 ").\n",
                           mxv->reserve_cues_space, cues_end - cuespos);
                    return AVERROR(EINVAL);
                }

                if (cues_end < cuespos + mxv->reserve_cues_space)
                    put_ebml_void(pb, mxv->reserve_cues_space -
                                  (cues_end - cuespos));

                avio_seek(pb, currentpos, SEEK_SET);
            } else {
                cuespos = mxv_write_cues(s, mxv->cues, mxv->tracks, s->nb_streams);
            }

            ret = mxv_add_seekhead_entry(mxv->seekhead, MXV_ID_CUES,
                                         cuespos);
            if (ret < 0)
                return ret;
        }

        mxv_write_seekhead(pb, mxv);

        // update the duration
        av_log(s, AV_LOG_DEBUG, "end duration = %" PRIu64 "\n", mxv->duration);
        currentpos = avio_tell(pb);
        avio_seek(mxv->info_bc, mxv->duration_offset, SEEK_SET);
        put_ebml_float(mxv->info_bc, MXV_ID_DURATION, mxv->duration);
        avio_seek(pb, mxv->info_pos, SEEK_SET);
        end_ebml_master_crc32(pb, &mxv->info_bc, mxv);

        // write tracks master
        avio_seek(pb, mxv->tracks_pos, SEEK_SET);
        end_ebml_master_crc32(pb, &mxv->tracks_bc, mxv);

        // update stream durations
        if (!mxv->is_live && mxv->stream_durations) {
            int i;
            int64_t curr = avio_tell(mxv->tags_bc);
            for (i = 0; i < s->nb_streams; ++i) {
                AVStream *st = s->streams[i];

                if (mxv->stream_duration_offsets[i] > 0) {
                    double duration_sec = mxv->stream_durations[i] * av_q2d(st->time_base);
                    char duration_string[20] = "";

                    av_log(s, AV_LOG_DEBUG, "stream %d end duration = %" PRIu64 "\n", i,
                           mxv->stream_durations[i]);

                    avio_seek(mxv->tags_bc, mxv->stream_duration_offsets[i], SEEK_SET);

                    snprintf(duration_string, 20, "%02d:%02d:%012.9f",
                             (int) duration_sec / 3600, ((int) duration_sec / 60) % 60,
                             fmod(duration_sec, 60));

                    put_ebml_binary(mxv->tags_bc, MXV_ID_TAGSTRING, duration_string, 20);
                }
            }
            avio_seek(mxv->tags_bc, curr, SEEK_SET);
        }
        if (mxv->tags_bc && !mxv->is_live) {
            avio_seek(pb, mxv->tags_pos, SEEK_SET);
            end_ebml_master_crc32(pb, &mxv->tags_bc, mxv);
        }

        avio_seek(pb, currentpos, SEEK_SET);
    }

    if (!mxv->is_live) {
        end_ebml_master(pb, mxv->segment);
    }

    mxv_free(mxv);
    return 0;
}

static int mxv_query_codec(enum AVCodecID codec_id, int std_compliance)
{
    int i;
    for (i = 0; ff_mxv_codec_tags[i].id != AV_CODEC_ID_NONE; i++)
        if (ff_mxv_codec_tags[i].id == codec_id)
            return 1;

    if (std_compliance < FF_COMPLIANCE_NORMAL) {
        enum AVMediaType type = avcodec_get_type(codec_id);
        // mxv theoretically supports any video/audio through VFW/ACM
        if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO)
            return 1;
    }

    return 0;
}

//static int webm_query_codec(enum AVCodecID codec_id, int std_compliance)
//{
//    int i;
//    for (i = 0; ff_webm_codec_tags[i].id != AV_CODEC_ID_NONE; i++)
//        if (ff_webm_codec_tags[i].id == codec_id)
//            return 1;
//
//    return 0;
//}

static int mxv_init(struct AVFormatContext *s)
{
    int i;

    if (s->nb_streams > MAX_TRACKS) {
        av_log(s, AV_LOG_ERROR,
               "At most %d streams are supported for muxing in MXV\n",
               MAX_TRACKS);
        return AVERROR(EINVAL);
    }

    for (i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->codecpar->codec_id == AV_CODEC_ID_ATRAC3 ||
            s->streams[i]->codecpar->codec_id == AV_CODEC_ID_COOK ||
            s->streams[i]->codecpar->codec_id == AV_CODEC_ID_RA_288 ||
            s->streams[i]->codecpar->codec_id == AV_CODEC_ID_SIPR ||
            s->streams[i]->codecpar->codec_id == AV_CODEC_ID_RV10 ||
            s->streams[i]->codecpar->codec_id == AV_CODEC_ID_RV20) {
            av_log(s, AV_LOG_ERROR,
                   "The MXV muxer does not yet support muxing %s\n",
                   avcodec_get_name(s->streams[i]->codecpar->codec_id));
            return AVERROR_PATCHWELCOME;
        }
    }

    if (s->avoid_negative_ts < 0) {
        s->avoid_negative_ts = 1;
        s->internal->avoid_negative_ts_use_pts = 1;
    }

    for (i = 0; i < s->nb_streams; i++) {
        // ms precision is the de-facto standard timescale for mxv files
        avpriv_set_pts_info(s->streams[i], 64, 1, 1000);
    }

    return 0;
}

static int mxv_check_bitstream(struct AVFormatContext *s, const AVPacket *pkt)
{
    int ret = 1;
    AVStream *st = s->streams[pkt->stream_index];

    if (st->codecpar->codec_id == AV_CODEC_ID_AAC) {
        if (pkt->size > 2 && (AV_RB16(pkt->data) & 0xfff0) == 0xfff0)
            ret = ff_stream_add_bitstream_filter(st, "aac_adtstoasc", NULL);
    } else if (st->codecpar->codec_id == AV_CODEC_ID_VP9) {
        ret = ff_stream_add_bitstream_filter(st, "vp9_superframe", NULL);
    }

    return ret;
}
#endif

static const AVCodecTag additional_audio_tags[] = {
    { AV_CODEC_ID_ALAC,      0XFFFFFFFF },
    { AV_CODEC_ID_MLP,       0xFFFFFFFF },
    { AV_CODEC_ID_OPUS,      0xFFFFFFFF },
    { AV_CODEC_ID_PCM_S16BE, 0xFFFFFFFF },
    { AV_CODEC_ID_PCM_S24BE, 0xFFFFFFFF },
    { AV_CODEC_ID_PCM_S32BE, 0xFFFFFFFF },
    { AV_CODEC_ID_QDMC,      0xFFFFFFFF },
    { AV_CODEC_ID_QDM2,      0xFFFFFFFF },
    { AV_CODEC_ID_RA_144,    0xFFFFFFFF },
    { AV_CODEC_ID_RA_288,    0xFFFFFFFF },
    { AV_CODEC_ID_COOK,      0xFFFFFFFF },
    { AV_CODEC_ID_TRUEHD,    0xFFFFFFFF },
    { AV_CODEC_ID_NONE,      0xFFFFFFFF }
};

static const AVCodecTag additional_video_tags[] = {
    { AV_CODEC_ID_RV10,      0xFFFFFFFF },
    { AV_CODEC_ID_RV20,      0xFFFFFFFF },
    { AV_CODEC_ID_RV30,      0xFFFFFFFF },
    { AV_CODEC_ID_NONE,      0xFFFFFFFF }
};

static const AVCodecTag additional_subtitle_tags[] = {
    { AV_CODEC_ID_DVB_SUBTITLE,      0xFFFFFFFF },
    { AV_CODEC_ID_DVD_SUBTITLE,      0xFFFFFFFF },
    { AV_CODEC_ID_HDMV_PGS_SUBTITLE, 0xFFFFFFFF },
    { AV_CODEC_ID_NONE,              0xFFFFFFFF }
};

#if CONFIG_MXV_MUXER
#if !CONFIG_MXV_FROM_MXVP
#define OFFSET(x) offsetof(MXVMuxContext, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "reserve_index_space", "Reserve a given amount of space (in bytes) at the beginning of the file for the index (cues).", OFFSET(reserve_cues_space), AV_OPT_TYPE_INT,   { .i64 = 0 },   0, INT_MAX,   FLAGS },
    { "cluster_size_limit",  "Store at most the provided amount of bytes in a cluster. ",                                     OFFSET(cluster_size_limit), AV_OPT_TYPE_INT  , { .i64 = -1 }, -1, INT_MAX,   FLAGS },
    { "cluster_time_limit",  "Store at most the provided number of milliseconds in a cluster.",                               OFFSET(cluster_time_limit), AV_OPT_TYPE_INT64, { .i64 = -1 }, -1, INT64_MAX, FLAGS },
    { "dash", "Create a WebM file conforming to WebM DASH specification", OFFSET(is_dash), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "dash_track_number", "Track number for the DASH stream", OFFSET(dash_track_number), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 127, FLAGS },
    { "live", "Write files assuming it is a live stream.", OFFSET(is_live), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "allow_raw_vfw", "allow RAW VFW mode", OFFSET(allow_raw_vfw), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "write_crc32", "write a CRC32 element inside every Level 1 element", OFFSET(write_crc), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { NULL },
};

static const AVClass mxv_class = {
    .class_name = "mxv muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVOutputFormat ff_mxv_muxer = {
    .name              = "mxv",
    .long_name         = NULL_IF_CONFIG_SMALL("MXV"),
    .mime_type         = "video/x-mxv",
    .extensions        = "mxv",
    .priv_data_size    = sizeof(MXVMuxContext),
    .audio_codec       = CONFIG_LIBVORBIS_ENCODER ?
                         AV_CODEC_ID_VORBIS : AV_CODEC_ID_AC3,
    .video_codec       = CONFIG_LIBX264_ENCODER ?
                         AV_CODEC_ID_H264 : AV_CODEC_ID_MPEG4,
    .init              = mxv_init,
    .write_header      = mxv_write_header,
    .write_packet      = mxv_write_flush_packet,
    .write_trailer     = mxv_write_trailer,
    .flags             = AVFMT_GLOBALHEADER | AVFMT_VARIABLE_FPS |
                         AVFMT_TS_NONSTRICT | AVFMT_ALLOW_FLUSH,
    .codec_tag         = (const AVCodecTag* const []){
         ff_codec_bmp_tags, ff_codec_wav_tags,
         additional_audio_tags, additional_video_tags, additional_subtitle_tags, 0
    },
    .subtitle_codec    = AV_CODEC_ID_ASS,
    .query_codec       = mxv_query_codec,
    .check_bitstream   = mxv_check_bitstream,
    .priv_class        = &mxv_class,
};
#else
#include "mxv_wrap.h"

static int wrapper_mxv_init( AVFormatContext *s )
{
    return mxv_init( s );
}

static int wrapper_mxv_write_header( AVFormatContext *s )
{
    return mxv_write_header( s );
}

static int wrapper_mxv_write_flush_packet( AVFormatContext *s, AVPacket *pkt )
{
    return mxv_write_flush_packet( s, pkt );
}

static int wrapper_mxv_write_trailer( AVFormatContext *s )
{
    return mxv_write_trailer( s );
}

static int wrapper_mxv_query_codec( enum AVCodecID codec_id, int std_compliance )
{
    return mxv_query_codec( codec_id, std_compliance );
}

static int wrapper_mxv_check_bitstream( AVFormatContext *s, const AVPacket *pkt )
{
    return mxv_check_bitstream( s, pkt );
}

AVOutputFormat ff_mxv_muxer = {
    .name              = "mxv",
    .long_name         = NULL_IF_CONFIG_SMALL("MXV"),
    .mime_type         = "video/x-mxv",
    .extensions        = "mxv",
    .priv_data_size    = 10240,
    .audio_codec       = CONFIG_LIBVORBIS_ENCODER ?
                         AV_CODEC_ID_VORBIS : AV_CODEC_ID_AC3,
    .video_codec       = CONFIG_LIBX264_ENCODER ?
                         AV_CODEC_ID_H264 : AV_CODEC_ID_MPEG4,
    .init              = wrapper_mxv_init,
    .write_header      = wrapper_mxv_write_header,
    .write_packet      = wrapper_mxv_write_flush_packet,
    .write_trailer     = wrapper_mxv_write_trailer,
    .flags             = AVFMT_GLOBALHEADER | AVFMT_VARIABLE_FPS |
                         AVFMT_TS_NONSTRICT | AVFMT_ALLOW_FLUSH,
    .codec_tag         = (const AVCodecTag* const []){
         ff_codec_bmp_tags, ff_codec_wav_tags,
         additional_audio_tags, additional_video_tags, additional_subtitle_tags, 0
    },
    .subtitle_codec    = AV_CODEC_ID_ASS,
    .query_codec       = wrapper_mxv_query_codec,
    .check_bitstream   = wrapper_mxv_check_bitstream,
    //.priv_class        = &mxv_class,
};
#endif
#endif