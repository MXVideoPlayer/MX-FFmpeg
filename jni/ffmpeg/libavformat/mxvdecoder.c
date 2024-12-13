//
//  mxvdecoder.c
/*
 * downloadhttp.c
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

#include "config.h"

#include <inttypes.h>
#include <stdio.h>

#include "libavutil/avstring.h"
#include "libavutil/base64.h"
#include "libavutil/dict.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/lzo.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/time_internal.h"
#include "libavutil/spherical.h"

#include "libavcodec/bytestream.h"
#include "libavcodec/flac.h"
#include "libavcodec/mpeg4audio.h"

#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"
#include "isom.h"
#include "mxv.h"
#include "oggdec.h"
/* For ff_codec_get_id(). */
#include "riff.h"
#include "rmsipr.h"

#if CONFIG_BZLIB
#include <bzlib.h>
#endif
#if CONFIG_ZLIB
#include <zlib.h>
#endif

#include "qtpalette.h"

#if !CONFIG_MXV_FROM_MXVP

#define EBML_UNKNOWN_LENGTH  UINT64_MAX /* EBML unknown length, in uint64_t */
#define NEEDS_CHECKING                2 /* Indicates that some error checks
                                         * still need to be performed */
#define LEVEL_ENDED                   3 /* return value of ebml_parse when the
                                         * syntax level used for parsing ended. */
#define SKIP_THRESHOLD      1024 * 1024 /* In non-seekable mode, if more than SKIP_THRESHOLD
                                         * of unkown, potentially damaged data is encountered,
                                         * it is considered an error. */
#define UNKNOWN_EQUIV         50 * 1024 /* An unknown element is considered equivalent
                                         * to this many bytes of unknown data for the
                                         * SKIP_THRESHOLD check. */

typedef enum {
    EBML_NONE,
    EBML_UINT,
    EBML_SINT,
    EBML_FLOAT,
    EBML_STR,
    EBML_UTF8,
    EBML_BIN,
    EBML_NEST,
    EBML_LEVEL1,
    EBML_STOP,
    EBML_TYPE_COUNT
} EbmlType;

typedef const struct EbmlSyntax {
    uint32_t id;
    EbmlType type;
    int list_elem_size;
    int data_offset;
    union {
        int64_t     i;
        uint64_t    u;
        double      f;
        const char *s;
        const struct EbmlSyntax *n;
    } def;
} EbmlSyntax;

typedef struct EbmlList {
    int nb_elem;
    void *elem;
} EbmlList;

typedef struct EbmlBin {
    int      size;
    AVBufferRef *buf;
    uint8_t *data;
    int64_t  pos;
} EbmlBin;

typedef struct Ebml {
    uint64_t version;
    uint64_t max_size;
    uint64_t id_length;
    char    *doctype;
    uint64_t doctype_version;
} Ebml;

typedef struct MXVTrackCompression {
    uint64_t algo;
    EbmlBin  settings;
} MXVTrackCompression;

typedef struct MXVTrackEncryption {
    uint64_t algo;
    EbmlBin  key_id;
} MXVTrackEncryption;

typedef struct MXVTrackEncoding {
    uint64_t scope;
    uint64_t type;
    MXVTrackCompression compression;
    MXVTrackEncryption encryption;
} MXVTrackEncoding;

typedef struct MXVMasteringMeta {
    double r_x;
    double r_y;
    double g_x;
    double g_y;
    double b_x;
    double b_y;
    double white_x;
    double white_y;
    double max_luminance;
    double min_luminance;
} MXVMasteringMeta;

typedef struct MXVTrackVideoColor {
    uint64_t matrix_coefficients;
    uint64_t bits_per_channel;
    uint64_t chroma_sub_horz;
    uint64_t chroma_sub_vert;
    uint64_t cb_sub_horz;
    uint64_t cb_sub_vert;
    uint64_t chroma_siting_horz;
    uint64_t chroma_siting_vert;
    uint64_t range;
    uint64_t transfer_characteristics;
    uint64_t primaries;
    uint64_t max_cll;
    uint64_t max_fall;
    MXVMasteringMeta mastering_meta;
} MXVTrackVideoColor;

typedef struct MXVTrackVideoProjection {
    uint64_t type;
    EbmlBin private;
    double yaw;
    double pitch;
    double roll;
} MXVTrackVideoProjection;

typedef struct MXVTrackVideo {
    double   frame_rate;
    uint64_t display_width;
    uint64_t display_height;
    uint64_t pixel_width;
    uint64_t pixel_height;
    EbmlBin  color_space;
    uint64_t display_unit;
    uint64_t interlaced;
    uint64_t field_order;
    uint64_t stereo_mode;
    uint64_t alpha_mode;
    EbmlList color;
    MXVTrackVideoProjection projection;
} MXVTrackVideo;

typedef struct MXVTrackAudio {
    double   samplerate;
    double   out_samplerate;
    uint64_t bitdepth;
    uint64_t channels;

    /* real audio header (extracted from extradata) */
    int      coded_framesize;
    int      sub_packet_h;
    int      frame_size;
    int      sub_packet_size;
    int      sub_packet_cnt;
    int      pkt_cnt;
    uint64_t buf_timecode;
    uint8_t *buf;
} MXVTrackAudio;

typedef struct MXVTrackPlane {
    uint64_t uid;
    uint64_t type;
} MXVTrackPlane;

typedef struct MXVTrackOperation {
    EbmlList combine_planes;
} MXVTrackOperation;

typedef struct MXVTrack {
    uint64_t num;
    uint64_t uid;
    uint64_t type;
    char    *name;
    char    *codec_id;
    EbmlBin  codec_priv;
    char    *language;
    double time_scale;
    uint64_t default_duration;
    uint64_t flag_default;
    uint64_t flag_forced;
    uint64_t seek_preroll;
    MXVTrackVideo video;
    MXVTrackAudio audio;
    MXVTrackOperation operation;
    EbmlList encodings;
    uint64_t codec_delay;
    uint64_t codec_delay_in_track_tb;

    AVStream *stream;
    int64_t end_timecode;
    int ms_compat;
    uint64_t max_block_additional_id;

    uint32_t palette[AVPALETTE_COUNT];
    int has_palette;
} MXVTrack;

typedef struct MXVAttachment {
    uint64_t uid;
    char *filename;
    char *mime;
    EbmlBin bin;

    AVStream *stream;
} MXVAttachment;

typedef struct MXVChapter {
    uint64_t start;
    uint64_t end;
    uint64_t uid;
    char    *title;

    AVChapter *chapter;
} MXVChapter;

typedef struct MXVIndexPos {
    uint64_t track;
    uint64_t pos;
} MXVIndexPos;

typedef struct MXVIndex {
    uint64_t time;
    EbmlList pos;
} MXVIndex;

typedef struct MXVTag {
    char *name;
    char *string;
    char *lang;
    uint64_t def;
    EbmlList sub;
} MXVTag;

typedef struct MXVTagTarget {
    char    *type;
    uint64_t typevalue;
    uint64_t trackuid;
    uint64_t chapteruid;
    uint64_t attachuid;
} MXVTagTarget;

typedef struct MXVTags {
    MXVTagTarget target;
    EbmlList tag;
} MXVTags;

typedef struct MXVSeekhead {
    uint64_t id;
    uint64_t pos;
} MXVSeekhead;

typedef struct MXVLevel {
    uint64_t start;
    uint64_t length;
} MXVLevel;

typedef struct MXVBlock {
    uint64_t duration;
    int64_t  reference;
    uint64_t non_simple;
    EbmlBin  bin;
    uint64_t additional_id;
    EbmlBin  additional;
    int64_t  discard_padding;
} MXVBlock;

typedef struct MXVCluster {
    MXVBlock block;
    uint64_t timecode;
    int64_t pos;
} MXVCluster;

typedef struct MXVLevel1Element {
    int64_t  pos;
    uint32_t id;
    int parsed;
} MXVLevel1Element;

typedef struct MXVDemuxContext {
    const AVClass *class;
    AVFormatContext *ctx;

    /* EBML stuff */
    MXVLevel levels[EBML_MAX_DEPTH];
    int      num_levels;
    uint32_t current_id;
    int64_t  resync_pos;
    int      unknown_count;

    uint64_t time_scale;
    double   duration;
    char    *title;
    char    *muxingapp;
    EbmlBin  date_utc;
    EbmlList tracks;
    EbmlList attachments;
    EbmlList chapters;
    EbmlList index;
    EbmlList tags;
    EbmlList seekhead;

    /* byte position of the segment inside the stream */
    int64_t segment_start;

    /* the packet queue */
    AVPacketList *queue;
    AVPacketList *queue_end;

    int done;

    /* What to skip before effectively reading a packet. */
    int skip_to_keyframe;
    uint64_t skip_to_timecode;

    /* File has a CUES element, but we defer parsing until it is needed. */
    int cues_parsing_deferred;

    /* Level1 elements and whether they were read yet */
    MXVLevel1Element level1_elems[64];
    int num_level1_elems;

    MXVCluster current_cluster;

    /* WebM DASH Manifest live flag */
    int is_live;

    /* Bandwidth value for WebM DASH Manifest */
    int bandwidth;
    uint8_t *aes_key;
} MXVDemuxContext;

#define CHILD_OF(parent) { .def = { .n = parent } }

// The following forward declarations need their size because
// a tentative definition with internal linkage must not be an
// incomplete type (6.7.2 in C90, 6.9.2 in C99).
// Removing the sizes breaks MSVC.
static EbmlSyntax ebml_syntax[3], mxv_segment[9], mxv_track_video_color[15], mxv_track_video[19],
                  mxv_track[27], mxv_track_encoding[6], mxv_track_encodings[2],
                  mxv_track_combine_planes[2], mxv_track_operation[2], mxv_tracks[2],
                  mxv_attachments[2], mxv_chapter_entry[9], mxv_chapter[6], mxv_chapters[2],
                  mxv_index_entry[3], mxv_index[2], mxv_tag[3], mxv_tags[2], mxv_seekhead[2],
                  mxv_blockadditions[2], mxv_blockgroup[8], mxv_cluster_parsing[8];

static EbmlSyntax ebml_header[] = {
    { EBML_ID_EBMLREADVERSION,    EBML_UINT, 0, offsetof(Ebml, version),         { .u = EBML_VERSION } },
    { EBML_ID_EBMLMAXSIZELENGTH,  EBML_UINT, 0, offsetof(Ebml, max_size),        { .u = 8 } },
    { EBML_ID_EBMLMAXIDLENGTH,    EBML_UINT, 0, offsetof(Ebml, id_length),       { .u = 4 } },
    { EBML_ID_DOCTYPE,            EBML_STR,  0, offsetof(Ebml, doctype),         { .s = "(none)" } },
    { EBML_ID_DOCTYPEREADVERSION, EBML_UINT, 0, offsetof(Ebml, doctype_version), { .u = 1 } },
    { EBML_ID_EBMLVERSION,        EBML_NONE },
    { EBML_ID_DOCTYPEVERSION,     EBML_NONE },
    CHILD_OF(ebml_syntax)
};

static EbmlSyntax ebml_syntax[] = {
    { EBML_ID_HEADER,      EBML_NEST, 0, 0, { .n = ebml_header } },
    { MXV_ID_SEGMENT, EBML_STOP },
    { 0 }
};

static EbmlSyntax mxv_info[] = {
    { MXV_ID_TIMECODESCALE, EBML_UINT,  0, offsetof(MXVDemuxContext, time_scale), { .u = 1000000 } },
    { MXV_ID_DURATION,      EBML_FLOAT, 0, offsetof(MXVDemuxContext, duration) },
    { MXV_ID_TITLE,         EBML_UTF8,  0, offsetof(MXVDemuxContext, title) },
    { MXV_ID_WRITINGAPP,    EBML_NONE },
    { MXV_ID_MUXINGAPP,     EBML_UTF8, 0, offsetof(MXVDemuxContext, muxingapp) },
    { MXV_ID_DATEUTC,       EBML_BIN,  0, offsetof(MXVDemuxContext, date_utc) },
    { MXV_ID_SEGMENTUID,    EBML_NONE },
    CHILD_OF(mxv_segment)
};

static EbmlSyntax mxv_mastering_meta[] = {
    { MXV_ID_VIDEOCOLOR_RX, EBML_FLOAT, 0, offsetof(MXVMasteringMeta, r_x), { .f=-1 } },
    { MXV_ID_VIDEOCOLOR_RY, EBML_FLOAT, 0, offsetof(MXVMasteringMeta, r_y), { .f=-1 } },
    { MXV_ID_VIDEOCOLOR_GX, EBML_FLOAT, 0, offsetof(MXVMasteringMeta, g_x), { .f=-1 } },
    { MXV_ID_VIDEOCOLOR_GY, EBML_FLOAT, 0, offsetof(MXVMasteringMeta, g_y), { .f=-1 } },
    { MXV_ID_VIDEOCOLOR_BX, EBML_FLOAT, 0, offsetof(MXVMasteringMeta, b_x), { .f=-1 } },
    { MXV_ID_VIDEOCOLOR_BY, EBML_FLOAT, 0, offsetof(MXVMasteringMeta, b_y), { .f=-1 } },
    { MXV_ID_VIDEOCOLOR_WHITEX, EBML_FLOAT, 0, offsetof(MXVMasteringMeta, white_x), { .f=-1 } },
    { MXV_ID_VIDEOCOLOR_WHITEY, EBML_FLOAT, 0, offsetof(MXVMasteringMeta, white_y), { .f=-1 } },
    { MXV_ID_VIDEOCOLOR_LUMINANCEMIN, EBML_FLOAT, 0, offsetof(MXVMasteringMeta, min_luminance), { .f=-1 } },
    { MXV_ID_VIDEOCOLOR_LUMINANCEMAX, EBML_FLOAT, 0, offsetof(MXVMasteringMeta, max_luminance), { .f=-1 } },
    CHILD_OF(mxv_track_video_color)
};

static EbmlSyntax mxv_track_video_color[] = {
    { MXV_ID_VIDEOCOLORMATRIXCOEFF,      EBML_UINT, 0, offsetof(MXVTrackVideoColor, matrix_coefficients), { .u = AVCOL_SPC_UNSPECIFIED } },
    { MXV_ID_VIDEOCOLORBITSPERCHANNEL,   EBML_UINT, 0, offsetof(MXVTrackVideoColor, bits_per_channel), { .u=0 } },
    { MXV_ID_VIDEOCOLORCHROMASUBHORZ,    EBML_UINT, 0, offsetof(MXVTrackVideoColor, chroma_sub_horz), { .u=0 } },
    { MXV_ID_VIDEOCOLORCHROMASUBVERT,    EBML_UINT, 0, offsetof(MXVTrackVideoColor, chroma_sub_vert), { .u=0 } },
    { MXV_ID_VIDEOCOLORCBSUBHORZ,        EBML_UINT, 0, offsetof(MXVTrackVideoColor, cb_sub_horz), { .u=0 } },
    { MXV_ID_VIDEOCOLORCBSUBVERT,        EBML_UINT, 0, offsetof(MXVTrackVideoColor, cb_sub_vert), { .u=0 } },
    { MXV_ID_VIDEOCOLORCHROMASITINGHORZ, EBML_UINT, 0, offsetof(MXVTrackVideoColor, chroma_siting_horz), { .u = MXV_COLOUR_CHROMASITINGHORZ_UNDETERMINED } },
    { MXV_ID_VIDEOCOLORCHROMASITINGVERT, EBML_UINT, 0, offsetof(MXVTrackVideoColor, chroma_siting_vert), { .u = MXV_COLOUR_CHROMASITINGVERT_UNDETERMINED } },
    { MXV_ID_VIDEOCOLORRANGE,            EBML_UINT, 0, offsetof(MXVTrackVideoColor, range), { .u = AVCOL_RANGE_UNSPECIFIED } },
    { MXV_ID_VIDEOCOLORTRANSFERCHARACTERISTICS, EBML_UINT, 0, offsetof(MXVTrackVideoColor, transfer_characteristics), { .u = AVCOL_TRC_UNSPECIFIED } },
    { MXV_ID_VIDEOCOLORPRIMARIES,        EBML_UINT, 0, offsetof(MXVTrackVideoColor, primaries), { .u = AVCOL_PRI_UNSPECIFIED } },
    { MXV_ID_VIDEOCOLORMAXCLL,           EBML_UINT, 0, offsetof(MXVTrackVideoColor, max_cll), { .u=0 } },
    { MXV_ID_VIDEOCOLORMAXFALL,          EBML_UINT, 0, offsetof(MXVTrackVideoColor, max_fall), { .u=0 } },
    { MXV_ID_VIDEOCOLORMASTERINGMETA,    EBML_NEST, 0, offsetof(MXVTrackVideoColor, mastering_meta), { .n = mxv_mastering_meta } },
    CHILD_OF(mxv_track_video)
};

static EbmlSyntax mxv_track_video_projection[] = {
    { MXV_ID_VIDEOPROJECTIONTYPE,        EBML_UINT,  0, offsetof(MXVTrackVideoProjection, type), { .u = MXV_VIDEO_PROJECTION_TYPE_RECTANGULAR } },
    { MXV_ID_VIDEOPROJECTIONPRIVATE,     EBML_BIN,   0, offsetof(MXVTrackVideoProjection, private) },
    { MXV_ID_VIDEOPROJECTIONPOSEYAW,     EBML_FLOAT, 0, offsetof(MXVTrackVideoProjection, yaw), { .f=0.0 } },
    { MXV_ID_VIDEOPROJECTIONPOSEPITCH,   EBML_FLOAT, 0, offsetof(MXVTrackVideoProjection, pitch), { .f=0.0 } },
    { MXV_ID_VIDEOPROJECTIONPOSEROLL,    EBML_FLOAT, 0, offsetof(MXVTrackVideoProjection, roll), { .f=0.0 } },
    CHILD_OF(mxv_track_video)
};

static EbmlSyntax mxv_track_video[] = {
    { MXV_ID_VIDEOFRAMERATE,      EBML_FLOAT, 0, offsetof(MXVTrackVideo, frame_rate) },
    { MXV_ID_VIDEODISPLAYWIDTH,   EBML_UINT,  0, offsetof(MXVTrackVideo, display_width), { .u=-1 } },
    { MXV_ID_VIDEODISPLAYHEIGHT,  EBML_UINT,  0, offsetof(MXVTrackVideo, display_height), { .u=-1 } },
    { MXV_ID_VIDEOPIXELWIDTH,     EBML_UINT,  0, offsetof(MXVTrackVideo, pixel_width) },
    { MXV_ID_VIDEOPIXELHEIGHT,    EBML_UINT,  0, offsetof(MXVTrackVideo, pixel_height) },
    { MXV_ID_VIDEOCOLORSPACE,     EBML_BIN,   0, offsetof(MXVTrackVideo, color_space) },
    { MXV_ID_VIDEOALPHAMODE,      EBML_UINT,  0, offsetof(MXVTrackVideo, alpha_mode) },
    { MXV_ID_VIDEOCOLOR,          EBML_NEST,  sizeof(MXVTrackVideoColor), offsetof(MXVTrackVideo, color), { .n = mxv_track_video_color } },
    { MXV_ID_VIDEOPROJECTION,     EBML_NEST,  0, offsetof(MXVTrackVideo, projection), { .n = mxv_track_video_projection } },
    { MXV_ID_VIDEOPIXELCROPB,     EBML_NONE },
    { MXV_ID_VIDEOPIXELCROPT,     EBML_NONE },
    { MXV_ID_VIDEOPIXELCROPL,     EBML_NONE },
    { MXV_ID_VIDEOPIXELCROPR,     EBML_NONE },
    { MXV_ID_VIDEODISPLAYUNIT,    EBML_UINT,  0, offsetof(MXVTrackVideo, display_unit), { .u= MXV_VIDEO_DISPLAYUNIT_PIXELS } },
    { MXV_ID_VIDEOFLAGINTERLACED, EBML_UINT,  0, offsetof(MXVTrackVideo, interlaced),  { .u = MXV_VIDEO_INTERLACE_FLAG_UNDETERMINED } },
    { MXV_ID_VIDEOFIELDORDER,     EBML_UINT,  0, offsetof(MXVTrackVideo, field_order), { .u = MXV_VIDEO_FIELDORDER_UNDETERMINED } },
    { MXV_ID_VIDEOSTEREOMODE,     EBML_UINT,  0, offsetof(MXVTrackVideo, stereo_mode), { .u = MXV_VIDEO_STEREOMODE_TYPE_NB } },
    { MXV_ID_VIDEOASPECTRATIO,    EBML_NONE },
    CHILD_OF(mxv_track)
};

static EbmlSyntax mxv_track_audio[] = {
    { MXV_ID_AUDIOSAMPLINGFREQ,    EBML_FLOAT, 0, offsetof(MXVTrackAudio, samplerate), { .f = 8000.0 } },
    { MXV_ID_AUDIOOUTSAMPLINGFREQ, EBML_FLOAT, 0, offsetof(MXVTrackAudio, out_samplerate) },
    { MXV_ID_AUDIOBITDEPTH,        EBML_UINT,  0, offsetof(MXVTrackAudio, bitdepth) },
    { MXV_ID_AUDIOCHANNELS,        EBML_UINT,  0, offsetof(MXVTrackAudio, channels),   { .u = 1 } },
    CHILD_OF(mxv_track)
};

static EbmlSyntax mxv_track_encoding_compression[] = {
    { MXV_ID_ENCODINGCOMPALGO,     EBML_UINT, 0, offsetof(MXVTrackCompression, algo), { .u = 0 } },
    { MXV_ID_ENCODINGCOMPSETTINGS, EBML_BIN,  0, offsetof(MXVTrackCompression, settings) },
    CHILD_OF(mxv_track_encoding)
};

static EbmlSyntax mxv_track_encoding_encryption[] = {
    { MXV_ID_ENCODINGENCALGO,        EBML_UINT, 0, offsetof(MXVTrackEncryption,algo), {.u = 0} },
    { MXV_ID_ENCODINGENCKEYID,       EBML_BIN, 0, offsetof(MXVTrackEncryption,key_id) },
    { MXV_ID_ENCODINGENCAESSETTINGS, EBML_NONE },
    { MXV_ID_ENCODINGSIGALGO,        EBML_NONE },
    { MXV_ID_ENCODINGSIGHASHALGO,    EBML_NONE },
    { MXV_ID_ENCODINGSIGKEYID,       EBML_NONE },
    { MXV_ID_ENCODINGSIGNATURE,      EBML_NONE },
    CHILD_OF(mxv_track_encoding)
};
static EbmlSyntax mxv_track_encoding[] = {
    { MXV_ID_ENCODINGSCOPE,       EBML_UINT, 0, offsetof(MXVTrackEncoding, scope),       { .u = 1 } },
    { MXV_ID_ENCODINGTYPE,        EBML_UINT, 0, offsetof(MXVTrackEncoding, type),        { .u = 0 } },
    { MXV_ID_ENCODINGCOMPRESSION, EBML_NEST, 0, offsetof(MXVTrackEncoding, compression), { .n = mxv_track_encoding_compression } },
    { MXV_ID_ENCODINGENCRYPTION,  EBML_NEST, 0, offsetof(MXVTrackEncoding, encryption),  { .n = mxv_track_encoding_encryption } },
    { MXV_ID_ENCODINGORDER,       EBML_NONE },
    CHILD_OF(mxv_track_encodings)
};

static EbmlSyntax mxv_track_encodings[] = {
    { MXV_ID_TRACKCONTENTENCODING, EBML_NEST, sizeof(MXVTrackEncoding), offsetof(MXVTrack, encodings), { .n = mxv_track_encoding } },
    CHILD_OF(mxv_track)
};

static EbmlSyntax mxv_track_plane[] = {
    { MXV_ID_TRACKPLANEUID,  EBML_UINT, 0, offsetof(MXVTrackPlane,uid) },
    { MXV_ID_TRACKPLANETYPE, EBML_UINT, 0, offsetof(MXVTrackPlane,type) },
    CHILD_OF(mxv_track_combine_planes)
};

static EbmlSyntax mxv_track_combine_planes[] = {
    { MXV_ID_TRACKPLANE, EBML_NEST, sizeof(MXVTrackPlane), offsetof(MXVTrackOperation,combine_planes), {.n = mxv_track_plane} },
    CHILD_OF(mxv_track_operation)
};

static EbmlSyntax mxv_track_operation[] = {
    { MXV_ID_TRACKCOMBINEPLANES, EBML_NEST, 0, 0, {.n = mxv_track_combine_planes} },
    CHILD_OF(mxv_track)
};

static EbmlSyntax mxv_track[] = {
    { MXV_ID_TRACKNUMBER,           EBML_UINT,  0, offsetof(MXVTrack, num) },
    { MXV_ID_TRACKNAME,             EBML_UTF8,  0, offsetof(MXVTrack, name) },
    { MXV_ID_TRACKUID,              EBML_UINT,  0, offsetof(MXVTrack, uid) },
    { MXV_ID_TRACKTYPE,             EBML_UINT,  0, offsetof(MXVTrack, type) },
    { MXV_ID_CODECID,               EBML_STR,   0, offsetof(MXVTrack, codec_id) },
    { MXV_ID_CODECPRIVATE,          EBML_BIN,   0, offsetof(MXVTrack, codec_priv) },
    { MXV_ID_CODECDELAY,            EBML_UINT,  0, offsetof(MXVTrack, codec_delay) },
    { MXV_ID_TRACKLANGUAGE,         EBML_UTF8,  0, offsetof(MXVTrack, language),     { .s = "eng" } },
    { MXV_ID_TRACKDEFAULTDURATION,  EBML_UINT,  0, offsetof(MXVTrack, default_duration) },
    { MXV_ID_TRACKTIMECODESCALE,    EBML_FLOAT, 0, offsetof(MXVTrack, time_scale),   { .f = 1.0 } },
    { MXV_ID_TRACKFLAGDEFAULT,      EBML_UINT,  0, offsetof(MXVTrack, flag_default), { .u = 1 } },
    { MXV_ID_TRACKFLAGFORCED,       EBML_UINT,  0, offsetof(MXVTrack, flag_forced),  { .u = 0 } },
    { MXV_ID_TRACKVIDEO,            EBML_NEST,  0, offsetof(MXVTrack, video),        { .n = mxv_track_video } },
    { MXV_ID_TRACKAUDIO,            EBML_NEST,  0, offsetof(MXVTrack, audio),        { .n = mxv_track_audio } },
    { MXV_ID_TRACKOPERATION,        EBML_NEST,  0, offsetof(MXVTrack, operation),    { .n = mxv_track_operation } },
    { MXV_ID_TRACKCONTENTENCODINGS, EBML_NEST,  0, 0,                                     { .n = mxv_track_encodings } },
    { MXV_ID_TRACKMAXBLKADDID,      EBML_UINT,  0, offsetof(MXVTrack, max_block_additional_id) },
    { MXV_ID_SEEKPREROLL,           EBML_UINT,  0, offsetof(MXVTrack, seek_preroll) },
    { MXV_ID_TRACKFLAGENABLED,      EBML_NONE },
    { MXV_ID_TRACKFLAGLACING,       EBML_NONE },
    { MXV_ID_CODECNAME,             EBML_NONE },
    { MXV_ID_CODECDECODEALL,        EBML_NONE },
    { MXV_ID_CODECINFOURL,          EBML_NONE },
    { MXV_ID_CODECDOWNLOADURL,      EBML_NONE },
    { MXV_ID_TRACKMINCACHE,         EBML_NONE },
    { MXV_ID_TRACKMAXCACHE,         EBML_NONE },
    CHILD_OF(mxv_tracks)
};

static EbmlSyntax mxv_tracks[] = {
    { MXV_ID_TRACKENTRY, EBML_NEST, sizeof(MXVTrack), offsetof(MXVDemuxContext, tracks), { .n = mxv_track } },
    CHILD_OF(mxv_segment)
};

static EbmlSyntax mxv_attachment[] = {
    { MXV_ID_FILEUID,      EBML_UINT, 0, offsetof(MXVAttachment, uid) },
    { MXV_ID_FILENAME,     EBML_UTF8, 0, offsetof(MXVAttachment, filename) },
    { MXV_ID_FILEMIMETYPE, EBML_STR,  0, offsetof(MXVAttachment, mime) },
    { MXV_ID_FILEDATA,     EBML_BIN,  0, offsetof(MXVAttachment, bin) },
    { MXV_ID_FILEDESC,     EBML_NONE },
    CHILD_OF(mxv_attachments)
};

static EbmlSyntax mxv_attachments[] = {
    { MXV_ID_ATTACHEDFILE, EBML_NEST, sizeof(MXVAttachment), offsetof(MXVDemuxContext, attachments), { .n = mxv_attachment } },
    CHILD_OF(mxv_segment)
};

static EbmlSyntax mxv_chapter_display[] = {
    { MXV_ID_CHAPSTRING,  EBML_UTF8, 0, offsetof(MXVChapter, title) },
    { MXV_ID_CHAPLANG,    EBML_NONE },
    { MXV_ID_CHAPCOUNTRY, EBML_NONE },
    CHILD_OF(mxv_chapter_entry)
};

static EbmlSyntax mxv_chapter_entry[] = {
    { MXV_ID_CHAPTERTIMESTART,   EBML_UINT, 0, offsetof(MXVChapter, start), { .u = AV_NOPTS_VALUE } },
    { MXV_ID_CHAPTERTIMEEND,     EBML_UINT, 0, offsetof(MXVChapter, end),   { .u = AV_NOPTS_VALUE } },
    { MXV_ID_CHAPTERUID,         EBML_UINT, 0, offsetof(MXVChapter, uid) },
    { MXV_ID_CHAPTERDISPLAY,     EBML_NEST, 0,                        0,         { .n = mxv_chapter_display } },
    { MXV_ID_CHAPTERFLAGHIDDEN,  EBML_NONE },
    { MXV_ID_CHAPTERFLAGENABLED, EBML_NONE },
    { MXV_ID_CHAPTERPHYSEQUIV,   EBML_NONE },
    { MXV_ID_CHAPTERATOM,        EBML_NONE },
    CHILD_OF(mxv_chapter)
};

static EbmlSyntax mxv_chapter[] = {
    { MXV_ID_CHAPTERATOM,        EBML_NEST, sizeof(MXVChapter), offsetof(MXVDemuxContext, chapters), { .n = mxv_chapter_entry } },
    { MXV_ID_EDITIONUID,         EBML_NONE },
    { MXV_ID_EDITIONFLAGHIDDEN,  EBML_NONE },
    { MXV_ID_EDITIONFLAGDEFAULT, EBML_NONE },
    { MXV_ID_EDITIONFLAGORDERED, EBML_NONE },
    CHILD_OF(mxv_chapters)
};

static EbmlSyntax mxv_chapters[] = {
    { MXV_ID_EDITIONENTRY, EBML_NEST, 0, 0, { .n = mxv_chapter } },
    CHILD_OF(mxv_segment)
};

static EbmlSyntax mxv_index_pos[] = {
    { MXV_ID_CUETRACK,           EBML_UINT, 0, offsetof(MXVIndexPos, track) },
    { MXV_ID_CUECLUSTERPOSITION, EBML_UINT, 0, offsetof(MXVIndexPos, pos) },
    { MXV_ID_CUERELATIVEPOSITION,EBML_NONE },
    { MXV_ID_CUEDURATION,        EBML_NONE },
    { MXV_ID_CUEBLOCKNUMBER,     EBML_NONE },
    CHILD_OF(mxv_index_entry)
};

static EbmlSyntax mxv_index_entry[] = {
    { MXV_ID_CUETIME,          EBML_UINT, 0,                        offsetof(MXVIndex, time) },
    { MXV_ID_CUETRACKPOSITION, EBML_NEST, sizeof(MXVIndexPos), offsetof(MXVIndex, pos), { .n = mxv_index_pos } },
    CHILD_OF(mxv_index)
};

static EbmlSyntax mxv_index[] = {
    { MXV_ID_POINTENTRY, EBML_NEST, sizeof(MXVIndex), offsetof(MXVDemuxContext, index), { .n = mxv_index_entry } },
    CHILD_OF(mxv_segment)
};

static EbmlSyntax mxv_simpletag[] = {
    { MXV_ID_TAGNAME,        EBML_UTF8, 0,                   offsetof(MXVTag, name) },
    { MXV_ID_TAGSTRING,      EBML_UTF8, 0,                   offsetof(MXVTag, string) },
    { MXV_ID_TAGLANG,        EBML_STR,  0,                   offsetof(MXVTag, lang), { .s = "und" } },
    { MXV_ID_TAGDEFAULT,     EBML_UINT, 0,                   offsetof(MXVTag, def) },
    { MXV_ID_TAGDEFAULT_BUG, EBML_UINT, 0,                   offsetof(MXVTag, def) },
    { MXV_ID_SIMPLETAG,      EBML_NEST, sizeof(MXVTag), offsetof(MXVTag, sub),  { .n = mxv_simpletag } },
    CHILD_OF(mxv_tag)
};

static EbmlSyntax mxv_tagtargets[] = {
    { MXV_ID_TAGTARGETS_TYPE,       EBML_STR,  0, offsetof(MXVTagTarget, type) },
    { MXV_ID_TAGTARGETS_TYPEVALUE,  EBML_UINT, 0, offsetof(MXVTagTarget, typevalue), { .u = 50 } },
    { MXV_ID_TAGTARGETS_TRACKUID,   EBML_UINT, 0, offsetof(MXVTagTarget, trackuid) },
    { MXV_ID_TAGTARGETS_CHAPTERUID, EBML_UINT, 0, offsetof(MXVTagTarget, chapteruid) },
    { MXV_ID_TAGTARGETS_ATTACHUID,  EBML_UINT, 0, offsetof(MXVTagTarget, attachuid) },
    CHILD_OF(mxv_tag)
};

static EbmlSyntax mxv_tag[] = {
    { MXV_ID_SIMPLETAG,  EBML_NEST, sizeof(MXVTag), offsetof(MXVTags, tag),    { .n = mxv_simpletag } },
    { MXV_ID_TAGTARGETS, EBML_NEST, 0,                   offsetof(MXVTags, target), { .n = mxv_tagtargets } },
    CHILD_OF(mxv_tags)
};

static EbmlSyntax mxv_tags[] = {
    { MXV_ID_TAG, EBML_NEST, sizeof(MXVTags), offsetof(MXVDemuxContext, tags), { .n = mxv_tag } },
    CHILD_OF(mxv_segment)
};

static EbmlSyntax mxv_seekhead_entry[] = {
    { MXV_ID_SEEKID,       EBML_UINT, 0, offsetof(MXVSeekhead, id) },
    { MXV_ID_SEEKPOSITION, EBML_UINT, 0, offsetof(MXVSeekhead, pos), { .u = -1 } },
    CHILD_OF(mxv_seekhead)
};

static EbmlSyntax mxv_seekhead[] = {
    { MXV_ID_SEEKENTRY, EBML_NEST, sizeof(MXVSeekhead), offsetof(MXVDemuxContext, seekhead), { .n = mxv_seekhead_entry } },
    CHILD_OF(mxv_segment)
};

static EbmlSyntax mxv_segment[] = {
    { MXV_ID_CLUSTER,     EBML_STOP },
    { MXV_ID_INFO,        EBML_LEVEL1, 0, 0, { .n = mxv_info } },
    { MXV_ID_TRACKS,      EBML_LEVEL1, 0, 0, { .n = mxv_tracks } },
    { MXV_ID_ATTACHMENTS, EBML_LEVEL1, 0, 0, { .n = mxv_attachments } },
    { MXV_ID_CHAPTERS,    EBML_LEVEL1, 0, 0, { .n = mxv_chapters } },
    { MXV_ID_CUES,        EBML_LEVEL1, 0, 0, { .n = mxv_index } },
    { MXV_ID_TAGS,        EBML_LEVEL1, 0, 0, { .n = mxv_tags } },
    { MXV_ID_SEEKHEAD,    EBML_LEVEL1, 0, 0, { .n = mxv_seekhead } },
    { 0 }   /* We don't want to go back to level 0, so don't add the parent. */
};

static EbmlSyntax mxv_segments[] = {
    { MXV_ID_SEGMENT, EBML_NEST, 0, 0, { .n = mxv_segment } },
    { 0 }
};

static EbmlSyntax mxv_blockmore[] = {
    { MXV_ID_BLOCKADDID,      EBML_UINT, 0, offsetof(MXVBlock,additional_id) },
    { MXV_ID_BLOCKADDITIONAL, EBML_BIN,  0, offsetof(MXVBlock,additional) },
    CHILD_OF(mxv_blockadditions)
};

static EbmlSyntax mxv_blockadditions[] = {
    { MXV_ID_BLOCKMORE, EBML_NEST, 0, 0, {.n = mxv_blockmore} },
    CHILD_OF(mxv_blockgroup)
};

static EbmlSyntax mxv_blockgroup[] = {
    { MXV_ID_BLOCK,          EBML_BIN,  0, offsetof(MXVBlock, bin) },
    { MXV_ID_BLOCKADDITIONS, EBML_NEST, 0, 0, { .n = mxv_blockadditions} },
    { MXV_ID_BLOCKDURATION,  EBML_UINT, 0, offsetof(MXVBlock, duration) },
    { MXV_ID_DISCARDPADDING, EBML_SINT, 0, offsetof(MXVBlock, discard_padding) },
    { MXV_ID_BLOCKREFERENCE, EBML_SINT, 0, offsetof(MXVBlock, reference), { .i = INT64_MIN } },
    { MXV_ID_CODECSTATE,     EBML_NONE },
    {                          1, EBML_UINT, 0, offsetof(MXVBlock, non_simple), { .u = 1 } },
    CHILD_OF(mxv_cluster_parsing)
};

// The following array contains SimpleBlock and BlockGroup twice
// in order to reuse the other values for mxv_cluster_enter.
static EbmlSyntax mxv_cluster_parsing[] = {
    { MXV_ID_SIMPLEBLOCK,     EBML_BIN,  0, offsetof(MXVBlock, bin) },
    { MXV_ID_BLOCKGROUP,      EBML_NEST, 0, 0, { .n = mxv_blockgroup } },
    { MXV_ID_CLUSTERTIMECODE, EBML_UINT, 0, offsetof(MXVCluster, timecode) },
    { MXV_ID_SIMPLEBLOCK,     EBML_STOP },
    { MXV_ID_BLOCKGROUP,      EBML_STOP },
    { MXV_ID_CLUSTERPOSITION, EBML_NONE },
    { MXV_ID_CLUSTERPREVSIZE, EBML_NONE },
    CHILD_OF(mxv_segment)
};

static EbmlSyntax mxv_cluster_enter[] = {
    { MXV_ID_CLUSTER,     EBML_NEST, 0, 0, { .n = &mxv_cluster_parsing[2] } },
    { 0 }
};
#undef CHILD_OF

static const char *const mxv_doctypes[] = { "mxv", "webm" };

static int mxv_read_close(AVFormatContext *s);

/*
 * This function prepares the status for parsing of level 1 elements.
 */
static int mxv_reset_status(MXVDemuxContext *mxv,
                                 uint32_t id, int64_t position)
{
    if (position >= 0) {
        int err = avio_seek(mxv->ctx->pb, position, SEEK_SET);
        if (err < 0)
            return err;
    }

    mxv->current_id    = id;
    mxv->num_levels    = 1;
    mxv->unknown_count = 0;
    mxv->resync_pos = avio_tell(mxv->ctx->pb);
    if (id)
        mxv->resync_pos -= (av_log2(id) + 7) / 8;

    return 0;
}

static int mxv_resync(MXVDemuxContext *mxv, int64_t last_pos)
{
    AVIOContext *pb = mxv->ctx->pb;
    uint32_t id;

    /* Try to seek to the last position to resync from. If this doesn't work,
     * we resync from the earliest position available: The start of the buffer. */
    if (last_pos < avio_tell(pb) && avio_seek(pb, last_pos + 1, SEEK_SET) < 0) {
        av_log(mxv->ctx, AV_LOG_WARNING,
               "Seek to desired resync point failed. Seeking to "
               "earliest point available instead.\n");
        avio_seek(pb, FFMAX(avio_tell(pb) + (pb->buffer - pb->buf_ptr),
                            last_pos + 1), SEEK_SET);
    }

    id = avio_rb32(pb);

    // try to find a toplevel element
    while (!avio_feof(pb)) {
        if (id == MXV_ID_INFO     || id == MXV_ID_TRACKS      ||
            id == MXV_ID_CUES     || id == MXV_ID_TAGS        ||
            id == MXV_ID_SEEKHEAD || id == MXV_ID_ATTACHMENTS ||
            id == MXV_ID_CLUSTER  || id == MXV_ID_CHAPTERS) {
            /* Prepare the context for parsing of a level 1 element. */
            mxv_reset_status(mxv, id, -1);
            /* Given that we are here means that an error has occured,
             * so treat the segment as unknown length in order not to
             * discard valid data that happens to be beyond the designated
             * end of the segment. */
            mxv->levels[0].length = EBML_UNKNOWN_LENGTH;
            return 0;
        }
        id = (id << 8) | avio_r8(pb);
    }

    mxv->done = 1;
    return pb->error ? pb->error : AVERROR_EOF;
}

/*
 * Read: an "EBML number", which is defined as a variable-length
 * array of bytes. The first byte indicates the length by giving a
 * number of 0-bits followed by a one. The position of the first
 * "one" bit inside the first byte indicates the length of this
 * number.
 * Returns: number of bytes read, < 0 on error
 */
static int ebml_read_num(MXVDemuxContext *mxv, AVIOContext *pb,
                         int max_size, uint64_t *number, int eof_forbidden)
{
    int read, n = 1;
    uint64_t total;
    int64_t pos;

    /* The first byte tells us the length in bytes - except when it is zero. */
    total = avio_r8(pb);
    if (pb->eof_reached)
        goto err;

    /* get the length of the EBML number */
    read = 8 - ff_log2_tab[total];

    if (!total || read > max_size) {
        pos = avio_tell(pb) - 1;
        if (!total) {
            av_log(mxv->ctx, AV_LOG_ERROR,
                   "0x00 at pos %"PRId64" (0x%"PRIx64") invalid as first byte "
                   "of an EBML number\n", pos, pos);
        } else {
            av_log(mxv->ctx, AV_LOG_ERROR,
                   "Length %d indicated by an EBML number's first byte 0x%02x "
                   "at pos %"PRId64" (0x%"PRIx64") exceeds max length %d.\n",
                   read, (uint8_t) total, pos, pos, max_size);
        }
        return AVERROR_INVALIDDATA;
    }

    /* read out length */
    total ^= 1 << ff_log2_tab[total];
    while (n++ < read)
        total = (total << 8) | avio_r8(pb);

    if (pb->eof_reached) {
        eof_forbidden = 1;
        goto err;
    }

    *number = total;

    return read;

err:
    pos = avio_tell(pb);
    if (pb->error) {
        av_log(mxv->ctx, AV_LOG_ERROR,
               "Read error at pos. %"PRIu64" (0x%"PRIx64")\n",
               pos, pos);
        return pb->error;
    }
    if (eof_forbidden) {
        av_log(mxv->ctx, AV_LOG_ERROR, "File ended prematurely "
               "at pos. %"PRIu64" (0x%"PRIx64")\n", pos, pos);
        return AVERROR(EIO);
    }
    return AVERROR_EOF;
}

/**
 * Read a EBML length value.
 * This needs special handling for the "unknown length" case which has multiple
 * encodings.
 */
static int ebml_read_length(MXVDemuxContext *mxv, AVIOContext *pb,
                            uint64_t *number)
{
    int res = ebml_read_num(mxv, pb, 8, number, 1);
    if (res > 0 && *number + 1 == 1ULL << (7 * res))
        *number = EBML_UNKNOWN_LENGTH;
    return res;
}

/*
 * Read the next element as an unsigned int.
 * Returns NEEDS_CHECKING.
 */
static int ebml_read_uint(AVIOContext *pb, int size, uint64_t *num)
{
    int n = 0;

    /* big-endian ordering; build up number */
    *num = 0;
    while (n++ < size)
        *num = (*num << 8) | avio_r8(pb);

    return NEEDS_CHECKING;
}

/*
 * Read the next element as a signed int.
 * Returns NEEDS_CHECKING.
 */
static int ebml_read_sint(AVIOContext *pb, int size, int64_t *num)
{
    int n = 1;

    if (size == 0) {
        *num = 0;
    } else {
        *num = sign_extend(avio_r8(pb), 8);

        /* big-endian ordering; build up number */
        while (n++ < size)
            *num = ((uint64_t)*num << 8) | avio_r8(pb);
    }

    return NEEDS_CHECKING;
}

/*
 * Read the next element as a float.
 * Returns NEEDS_CHECKING or < 0 on obvious failure.
 */
static int ebml_read_float(AVIOContext *pb, int size, double *num)
{
    if (size == 0)
        *num = 0;
    else if (size == 4)
        *num = av_int2float(avio_rb32(pb));
    else if (size == 8)
        *num = av_int2double(avio_rb64(pb));
    else
        return AVERROR_INVALIDDATA;

    return NEEDS_CHECKING;
}

/*
 * Read the next element as an ASCII string.
 * 0 is success, < 0 or NEEDS_CHECKING is failure.
 */
static int ebml_read_ascii(AVIOContext *pb, int size, char **str)
{
    char *res;
    int ret;

    /* EBML strings are usually not 0-terminated, so we allocate one
     * byte more, read the string and NULL-terminate it ourselves. */
    if (!(res = av_malloc(size + 1)))
        return AVERROR(ENOMEM);
    if ((ret = avio_read(pb, (uint8_t *) res, size)) != size) {
        av_free(res);
        return ret < 0 ? ret : NEEDS_CHECKING;
    }
    (res)[size] = '\0';
    av_free(*str);
    *str = res;

    return 0;
}

/*
 * Read the next element as binary data.
 * 0 is success, < 0 or NEEDS_CHECKING is failure.
 */
static int ebml_read_binary(AVIOContext *pb, int length,
                            int64_t pos, EbmlBin *bin)
{
    int ret;

    ret = av_buffer_realloc(&bin->buf, length + AV_INPUT_BUFFER_PADDING_SIZE);
    if (ret < 0)
        return ret;
    memset(bin->buf->data + length, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    bin->data = bin->buf->data;
    bin->size = length;
    bin->pos  = pos;
    if ((ret = avio_read(pb, bin->data, length)) != length) {
        av_buffer_unref(&bin->buf);
        bin->data = NULL;
        bin->size = 0;
        return ret < 0 ? ret : NEEDS_CHECKING;
    }

    return 0;
}

/*
 * Read the next element, but only the header. The contents
 * are supposed to be sub-elements which can be read separately.
 * 0 is success, < 0 is failure.
 */
static int ebml_read_master(MXVDemuxContext *mxv,
                            uint64_t length, int64_t pos)
{
    MXVLevel *level;

    if (mxv->num_levels >= EBML_MAX_DEPTH) {
        av_log(mxv->ctx, AV_LOG_ERROR,
               "File moves beyond max. allowed depth (%d)\n", EBML_MAX_DEPTH);
        return AVERROR(ENOSYS);
    }

    level         = &mxv->levels[mxv->num_levels++];
    level->start  = pos;
    level->length = length;

    return 0;
}

/*
 * Read signed/unsigned "EBML" numbers.
 * Return: number of bytes processed, < 0 on error
 */
static int mxv_ebmlnum_uint(MXVDemuxContext *mxv,
                                 uint8_t *data, uint32_t size, uint64_t *num)
{
    AVIOContext pb;
    ffio_init_context(&pb, data, size, 0, NULL, NULL, NULL, NULL);
    return ebml_read_num(mxv, &pb, FFMIN(size, 8), num, 1);
}

/*
 * Same as above, but signed.
 */
static int mxv_ebmlnum_sint(MXVDemuxContext *mxv,
                                 uint8_t *data, uint32_t size, int64_t *num)
{
    uint64_t unum;
    int res;

    /* read as unsigned number first */
    if ((res = mxv_ebmlnum_uint(mxv, data, size, &unum)) < 0)
        return res;

    /* make signed (weird way) */
    *num = unum - ((1LL << (7 * res - 1)) - 1);

    return res;
}

static int ebml_parse(MXVDemuxContext *mxv,
                      EbmlSyntax *syntax, void *data);

static EbmlSyntax *ebml_parse_id(EbmlSyntax *syntax, uint32_t id)
{
    int i;

    // Whoever touches this should be aware of the duplication
    // existing in mxv_cluster_parsing.
    for (i = 0; syntax[i].id; i++)
        if (id == syntax[i].id)
            break;

    return &syntax[i];
}

static int ebml_parse_nest(MXVDemuxContext *mxv, EbmlSyntax *syntax,
                           void *data)
{
    int res;

    if (data) {
        for (int i = 0; syntax[i].id; i++)
            switch (syntax[i].type) {
            case EBML_UINT:
                *(uint64_t *) ((char *) data + syntax[i].data_offset) = syntax[i].def.u;
                break;
            case EBML_SINT:
                *(int64_t *) ((char *) data + syntax[i].data_offset) = syntax[i].def.i;
                break;
            case EBML_FLOAT:
                *(double *) ((char *) data + syntax[i].data_offset) = syntax[i].def.f;
                break;
            case EBML_STR:
            case EBML_UTF8:
                // the default may be NULL
                if (syntax[i].def.s) {
                    uint8_t **dst = (uint8_t **) ((uint8_t *) data + syntax[i].data_offset);
                    *dst = av_strdup(syntax[i].def.s);
                    if (!*dst)
                        return AVERROR(ENOMEM);
                }
                break;
            }

        if (!mxv->levels[mxv->num_levels - 1].length) {
            mxv->num_levels--;
            return 0;
        }
    }

    do {
        res = ebml_parse(mxv, syntax, data);
    } while (!res);

    return res == LEVEL_ENDED ? 0 : res;
}

static int is_ebml_id_valid(uint32_t id)
{
    // Due to endian nonsense in MXV, the highest byte with any bits set
    // will contain the leading length bit. This bit in turn identifies the
    // total byte length of the element by its position within the byte.
    unsigned int bits = av_log2(id);
    return id && (bits + 7) / 8 ==  (8 - bits % 8);
}

/*
 * Allocate and return the entry for the level1 element with the given ID. If
 * an entry already exists, return the existing entry.
 */
static MXVLevel1Element *mxv_find_level1_elem(MXVDemuxContext *mxv,
                                                        uint32_t id)
{
    int i;
    MXVLevel1Element *elem;

    if (!is_ebml_id_valid(id))
        return NULL;

    // Some files link to all clusters; useless.
    if (id == MXV_ID_CLUSTER)
        return NULL;

    // There can be multiple seekheads.
    if (id != MXV_ID_SEEKHEAD) {
        for (i = 0; i < mxv->num_level1_elems; i++) {
            if (mxv->level1_elems[i].id == id)
                return &mxv->level1_elems[i];
        }
    }

    // Only a completely broken file would have more elements.
    // It also provides a low-effort way to escape from circular seekheads
    // (every iteration will add a level1 entry).
    if (mxv->num_level1_elems >= FF_ARRAY_ELEMS(mxv->level1_elems)) {
        av_log(mxv->ctx, AV_LOG_ERROR, "Too many level1 elements or circular seekheads.\n");
        return NULL;
    }

    elem = &mxv->level1_elems[mxv->num_level1_elems++];
    *elem = (MXVLevel1Element){.id = id};

    return elem;
}

static int ebml_parse(MXVDemuxContext *mxv,
                      EbmlSyntax *syntax, void *data)
{
    static const uint64_t max_lengths[EBML_TYPE_COUNT] = {
        // Forbid unknown-length EBML_NONE elements.
        [EBML_NONE]  = EBML_UNKNOWN_LENGTH - 1,
        [EBML_UINT]  = 8,
        [EBML_SINT]  = 8,
        [EBML_FLOAT] = 8,
        // max. 16 MB for strings
        [EBML_STR]   = 0x1000000,
        [EBML_UTF8]  = 0x1000000,
        // max. 256 MB for binary data
        [EBML_BIN]   = 0x10000000,
        // no limits for anything else
    };
    AVIOContext *pb = mxv->ctx->pb;
    uint32_t id;
    uint64_t length;
    int64_t pos = avio_tell(pb), pos_alt;
    int res, update_pos = 1, level_check;
    MXVLevel1Element *level1_elem;
    MXVLevel *level = mxv->num_levels ? &mxv->levels[mxv->num_levels - 1] : NULL;

    if (!mxv->current_id) {
        uint64_t id;
        res = ebml_read_num(mxv, pb, 4, &id, 0);
        if (res < 0) {
            if (pb->eof_reached && res == AVERROR_EOF) {
                if (mxv->is_live)
                    // in live mode, finish parsing if EOF is reached.
                    return 1;
                if (level && pos == avio_tell(pb)) {
                    if (level->length == EBML_UNKNOWN_LENGTH) {
                        // Unknown-length levels automatically end at EOF.
                        mxv->num_levels--;
                        return LEVEL_ENDED;
                    } else {
                        av_log(mxv->ctx, AV_LOG_ERROR, "File ended prematurely "
                               "at pos. %"PRIu64" (0x%"PRIx64")\n", pos, pos);
                    }
                }
            }
            return res;
        }
        mxv->current_id = id | 1 << 7 * res;
        pos_alt = pos + res;
    } else {
        pos_alt = pos;
        pos    -= (av_log2(mxv->current_id) + 7) / 8;
    }

    id = mxv->current_id;

    syntax = ebml_parse_id(syntax, id);
    if (!syntax->id && id != EBML_ID_VOID && id != EBML_ID_CRC32) {
        if (level && level->length == EBML_UNKNOWN_LENGTH) {
            // Unknown-length levels end when an element from an upper level
            // in the hierarchy is encountered.
            while (syntax->def.n) {
                syntax = ebml_parse_id(syntax->def.n, id);
                if (syntax->id) {
                    mxv->num_levels--;
                    return LEVEL_ENDED;
                }
            };
        }

        av_log(mxv->ctx, AV_LOG_DEBUG, "Unknown entry 0x%"PRIX32" at pos. "
                                            "%"PRId64"\n", id, pos);
        update_pos = 0; /* Don't update resync_pos as an error might have happened. */
    }

    if (data) {
        data = (char *) data + syntax->data_offset;
        if (syntax->list_elem_size) {
            EbmlList *list = data;
            void *newelem = av_realloc_array(list->elem, list->nb_elem + 1,
                                                   syntax->list_elem_size);
            if (!newelem)
                return AVERROR(ENOMEM);
            list->elem = newelem;
            data = (char *) list->elem + list->nb_elem * syntax->list_elem_size;
            memset(data, 0, syntax->list_elem_size);
            list->nb_elem++;
        }
    }

    if (syntax->type != EBML_STOP) {
        mxv->current_id = 0;
        if ((res = ebml_read_length(mxv, pb, &length)) < 0)
            return res;

        pos_alt += res;

        if (mxv->num_levels > 0) {
            if (length != EBML_UNKNOWN_LENGTH &&
                level->length != EBML_UNKNOWN_LENGTH) {
                uint64_t elem_end = pos_alt + length,
                        level_end = level->start + level->length;

                if (elem_end < level_end) {
                    level_check = 0;
                } else if (elem_end == level_end) {
                    level_check = LEVEL_ENDED;
                } else {
                    av_log(mxv->ctx, AV_LOG_ERROR,
                           "Element at 0x%"PRIx64" ending at 0x%"PRIx64" exceeds "
                           "containing master element ending at 0x%"PRIx64"\n",
                           pos, elem_end, level_end);
                    return AVERROR_INVALIDDATA;
                }
            } else if (length != EBML_UNKNOWN_LENGTH) {
                level_check = 0;
            } else if (level->length != EBML_UNKNOWN_LENGTH) {
                av_log(mxv->ctx, AV_LOG_ERROR, "Unknown-sized element "
                       "at 0x%"PRIx64" inside parent with finite size\n", pos);
                return AVERROR_INVALIDDATA;
            } else {
                level_check = 0;
                if (id != MXV_ID_CLUSTER && (syntax->type == EBML_LEVEL1
                                              ||  syntax->type == EBML_NEST)) {
                    // According to the current specifications only clusters and
                    // segments are allowed to be unknown-length. We also accept
                    // other unknown-length master elements.
                    av_log(mxv->ctx, AV_LOG_WARNING,
                           "Found unknown-length element 0x%"PRIX32" other than "
                           "a cluster at 0x%"PRIx64". Spec-incompliant, but "
                           "parsing will nevertheless be attempted.\n", id, pos);
                    update_pos = -1;
                }
            }
        } else
            level_check = 0;

        if (max_lengths[syntax->type] && length > max_lengths[syntax->type]) {
            if (length != EBML_UNKNOWN_LENGTH) {
                av_log(mxv->ctx, AV_LOG_ERROR,
                       "Invalid length 0x%"PRIx64" > 0x%"PRIx64" for element "
                       "with ID 0x%"PRIX32" at 0x%"PRIx64"\n",
                       length, max_lengths[syntax->type], id, pos);
            } else if (syntax->type != EBML_NONE) {
                av_log(mxv->ctx, AV_LOG_ERROR,
                       "Element with ID 0x%"PRIX32" at pos. 0x%"PRIx64" has "
                       "unknown length, yet the length of an element of its "
                       "type must be known.\n", id, pos);
            } else {
                av_log(mxv->ctx, AV_LOG_ERROR,
                       "Found unknown-length element with ID 0x%"PRIX32" at "
                       "pos. 0x%"PRIx64" for which no syntax for parsing is "
                       "available.\n", id, pos);
            }
            return AVERROR_INVALIDDATA;
        }

        if (!(pb->seekable & AVIO_SEEKABLE_NORMAL)) {
            // Loosing sync will likely manifest itself as encountering unknown
            // elements which are not reliably distinguishable from elements
            // belonging to future extensions of the format.
            // We use a heuristic to detect such situations: If the current
            // element is not expected at the current syntax level and there
            // were only a few unknown elements in a row, then the element is
            // skipped or considered defective based upon the length of the
            // current element (i.e. how much would be skipped); if there were
            // more than a few skipped elements in a row and skipping the current
            // element would lead us more than SKIP_THRESHOLD away from the last
            // known good position, then it is inferred that an error occured.
            // The dependency on the number of unknown elements in a row exists
            // because the distance to the last known good position is
            // automatically big if the last parsed element was big.
            // In both cases, each unknown element is considered equivalent to
            // UNKNOWN_EQUIV of skipped bytes for the check.
            // The whole check is only done for non-seekable output, because
            // in this situation skipped data can't simply be rechecked later.
            // This is especially important when using unkown length elements
            // as the check for whether a child exceeds its containing master
            // element is not effective in this situation.
            if (update_pos) {
                mxv->unknown_count = 0;
            } else {
                int64_t dist = length + UNKNOWN_EQUIV * mxv->unknown_count++;

                if (mxv->unknown_count > 3)
                    dist += pos_alt - mxv->resync_pos;

                if (dist > SKIP_THRESHOLD) {
                    av_log(mxv->ctx, AV_LOG_ERROR,
                           "Unknown element %"PRIX32" at pos. 0x%"PRIx64" with "
                           "length 0x%"PRIx64" considered as invalid data. Last "
                           "known good position 0x%"PRIx64", %d unknown elements"
                           " in a row\n", id, pos, length, mxv->resync_pos,
                           mxv->unknown_count);
                    return AVERROR_INVALIDDATA;
                }
            }
        }

        if (update_pos > 0) {
            // We have found an element that is allowed at this place
            // in the hierarchy and it passed all checks, so treat the beginning
            // of the element as the "last known good" position.
            mxv->resync_pos = pos;
        }

        if (!data && length != EBML_UNKNOWN_LENGTH)
            goto skip;
    }

    switch (syntax->type) {
    case EBML_UINT:
        res = ebml_read_uint(pb, length, data);
        break;
    case EBML_SINT:
        res = ebml_read_sint(pb, length, data);
        break;
    case EBML_FLOAT:
        res = ebml_read_float(pb, length, data);
        break;
    case EBML_STR:
    case EBML_UTF8:
        res = ebml_read_ascii(pb, length, data);
        break;
    case EBML_BIN:
        res = ebml_read_binary(pb, length, pos_alt, data);
        break;
    case EBML_LEVEL1:
    case EBML_NEST:
        if ((res = ebml_read_master(mxv, length, pos_alt)) < 0)
            return res;
        if (id == MXV_ID_SEGMENT)
            mxv->segment_start = pos_alt;
        if (id == MXV_ID_CUES)
            mxv->cues_parsing_deferred = 0;
        if (syntax->type == EBML_LEVEL1 &&
            (level1_elem = mxv_find_level1_elem(mxv, syntax->id))) {
            if (!level1_elem->pos) {
                // Zero is not a valid position for a level 1 element.
                level1_elem->pos = pos;
            } else if (level1_elem->pos != pos)
                av_log(mxv->ctx, AV_LOG_ERROR, "Duplicate element\n");
            level1_elem->parsed = 1;
        }
        if (res = ebml_parse_nest(mxv, syntax->def.n, data))
            return res;
        break;
    case EBML_STOP:
        return 1;
    skip:
    default:
        if (length) {
            int64_t res2;
            if (ffio_limit(pb, length) != length) {
                // ffio_limit emits its own error message,
                // so we don't have to.
                return AVERROR(EIO);
            }
            if ((res2 = avio_skip(pb, length - 1)) >= 0) {
                // avio_skip might take us past EOF. We check for this
                // by skipping only length - 1 bytes, reading a byte and
                // checking the error flags. This is done in order to check
                // that the element has been properly skipped even when
                // no filesize (that ffio_limit relies on) is available.
                avio_r8(pb);
                res = NEEDS_CHECKING;
            } else
                res = res2;
        } else
            res = 0;
    }
    if (res) {
        if (res == NEEDS_CHECKING) {
            if (pb->eof_reached) {
                if (pb->error)
                    res = pb->error;
                else
                    res = AVERROR_EOF;
            } else
                goto level_check;
        }

        if (res == AVERROR_INVALIDDATA)
            av_log(mxv->ctx, AV_LOG_ERROR, "Invalid element\n");
        else if (res == AVERROR(EIO))
            av_log(mxv->ctx, AV_LOG_ERROR, "Read error\n");
        else if (res == AVERROR_EOF) {
            av_log(mxv->ctx, AV_LOG_ERROR, "File ended prematurely\n");
            res = AVERROR(EIO);
        }

        return res;
    }

level_check:
    if (level_check == LEVEL_ENDED && mxv->num_levels) {
        level = &mxv->levels[mxv->num_levels - 1];
        pos   = avio_tell(pb);

        // Given that pos >= level->start no check for
        // level->length != EBML_UNKNOWN_LENGTH is necessary.
        while (mxv->num_levels && pos == level->start + level->length) {
            mxv->num_levels--;
            level--;
        }
    }

    return level_check;
}

static void ebml_free(EbmlSyntax *syntax, void *data)
{
    int i, j;
    for (i = 0; syntax[i].id; i++) {
        void *data_off = (char *) data + syntax[i].data_offset;
        switch (syntax[i].type) {
        case EBML_STR:
        case EBML_UTF8:
            av_freep(data_off);
            break;
        case EBML_BIN:
            av_buffer_unref(&((EbmlBin *) data_off)->buf);
            break;
        case EBML_LEVEL1:
        case EBML_NEST:
            if (syntax[i].list_elem_size) {
                EbmlList *list = data_off;
                char *ptr = list->elem;
                for (j = 0; j < list->nb_elem;
                     j++, ptr += syntax[i].list_elem_size)
                    ebml_free(syntax[i].def.n, ptr);
                av_freep(&list->elem);
                list->nb_elem = 0;
            } else
                ebml_free(syntax[i].def.n, data_off);
        default:
            break;
        }
    }
}

/*
 * Autodetecting...
 */
static int _has_pre_padding = 0;
static int mxv_prePadding_probe(const AVProbeData *p)
{
    int64_t offset = 0;
    uint32_t tag = 0;
    int has_pre_padding = 0, root_atoms_detect_all = 0, breakLoop = 0;

    while (offset < p->buf_size && !root_atoms_detect_all && !breakLoop) {
        int64_t size = 0;
        tag = AV_RL32(p->buf + offset + 4);
        switch(tag) {
        /* check for obvious tags */
        case MKTAG('m','x','v',' '):
        {
            has_pre_padding = 1;
            breakLoop = 1;
        }
            break;
        case MKTAG('f','t','y','p'):
        case MKTAG('p','d','i','n'):
        case MKTAG('m','o','o','v'):
        case MKTAG('m','o','o','f'):
        case MKTAG('m','f','r','a'):
        case MKTAG('f','r','e','e'):
        case MKTAG('s','k','i','p'):
        case MKTAG('j','u','n','k'):
        case MKTAG('w','i','d','e'):
        case MKTAG('p','n','o','t'):
        case MKTAG('p','i','c','t'):
        case MKTAG('m','e','t','a'):
        case MKTAG('m','e','c','o'):
        case MKTAG('u','u','i','d'): //Used by Sony's MSNV brand of MP4
        case MKTAG('m','d','a','t'):
            size = AV_RB32(p->buf + offset);
            break;
        default:
            root_atoms_detect_all = 1;
        }
        if ( size == 1) {
            //large size atom
            size = AV_RB64(p->buf+offset + 8);
        }
        offset += FFMAX(4, size);
    }
    return has_pre_padding;
}

//static int mxv_prePadding_size(const AVProbeData *p)
//{
//    int64_t offset = 0;
//    int64_t total_size = 0;
//    uint32_t tag;
//    int root_atoms_detect_all = 0;
//    while (offset < p->buf_size && !root_atoms_detect_all) {
//        int64_t size = 0;
//        tag = AV_RL32(p->buf + offset + 4);
//        switch(tag) {
//        /* check for obvious tags */
//        case MKTAG('f','t','y','p'):
//        case MKTAG('p','d','i','n'):
//        case MKTAG('m','o','o','v'):
//        case MKTAG('m','o','o','f'):
//        case MKTAG('m','f','r','a'):
//        case MKTAG('f','r','e','e'):
//        case MKTAG('s','k','i','p'):
//        case MKTAG('j','u','n','k'):
//        case MKTAG('w','i','d','e'):
//        case MKTAG('p','n','o','t'):
//        case MKTAG('p','i','c','t'):
//        case MKTAG('m','e','t','a'):
//        case MKTAG('m','e','c','o'):
//        case MKTAG('u','u','i','d'): //Used by Sony's MSNV brand of MP4
//        case MKTAG('m','d','a','t'):
//        case MKTAG('m','x','v',' '):
//            size = AV_RB32(p->buf + offset);
//            break;
//        default:
//            root_atoms_detect_all = 1;
//        }
//        if ( size == 1) {
//            //large size atom
//            size = AV_RB64(p->buf+offset + 8);
//        }
//        offset += size;
//    }
//    printf("------total size = %d--------\n",offset);
//    return offset;
//}

///known top levels find in link >>> https://wiki.multimedia.cx/index.php/QuickTime_container
static int mxv_prePadding_size(AVIOContext *pb)
{
    int64_t offset = 0;
    int64_t total_size = 0;
    uint32_t tag;
    int root_atoms_detect_all = 0;
    while (offset < avio_size(pb) && !root_atoms_detect_all) {
        int64_t size = 0;
        size = avio_rb32(pb);
        tag = avio_rl32(pb);
        if ( size == 1 ) {
            //large size atom
            size = avio_rb64(pb);
        }
        switch(tag) {
        /* check for obvious tags */
        case MKTAG('f','t','y','p'):
        case MKTAG('p','d','i','n'):
        case MKTAG('m','o','o','v'):
        case MKTAG('m','o','o','f'):
        case MKTAG('m','f','r','a'):
        case MKTAG('f','r','e','e'):
        case MKTAG('s','k','i','p'):
        case MKTAG('j','u','n','k'):
        case MKTAG('w','i','d','e'):
        case MKTAG('p','n','o','t'):
        case MKTAG('p','i','c','t'):
        case MKTAG('m','e','t','a'):
        case MKTAG('m','e','c','o'):
        case MKTAG('u','u','i','d'): //Used by Sony's MSNV brand of MP4
        case MKTAG('m','d','a','t'):
        case MKTAG('m','x','v',' '):
            offset += size;
            avio_seek(pb, offset, SEEK_SET);
            break;
        default:
            root_atoms_detect_all = 1;
        }
    }
    avio_seek(pb, 0, SEEK_SET);
    return offset;
}

static int mxv_probe(const AVProbeData *p)
{
    uint8_t *buffer = p->buf;
    int buffer_size = p->buf_size;
    _has_pre_padding = mxv_prePadding_probe(p);
    if (_has_pre_padding)
        return AVPROBE_SCORE_MAX;

    uint64_t total = 0;
    int len_mask = 0x80, size = 1, n = 1, i;

    /* EBML header? */
    if (AV_RB32(buffer) != EBML_ID_HEADER)
        return 0;

    /* length of header */
    total = buffer[4];
    while (size <= 8 && !(total & len_mask)) {
        size++;
        len_mask >>= 1;
    }
    if (size > 8)
        return 0;
    total &= (len_mask - 1);
    while (n < size)
        total = (total << 8) | buffer[4 + n++];

    if (total + 1 == 1ULL << (7 * size)){
        /* Unknown-length header - simply parse the whole buffer. */
        total = buffer_size - 4 - size;
    } else {
        /* Does the probe data contain the whole header? */
        if (buffer_size < 4 + size + total)
            return 0;
    }

    /* The header should contain a known document type. For now,
     * we don't parse the whole header but simply check for the
     * availability of that array of characters inside the header.
     * Not fully fool-proof, but good enough. */
    for (i = 0; i < FF_ARRAY_ELEMS(mxv_doctypes); i++) {
        size_t probelen = strlen(mxv_doctypes[i]);
        if (total < probelen)
            continue;
        for (n = 4 + size; n <= 4 + size + total - probelen; n++) {
            if (!memcmp(buffer + n, mxv_doctypes[i], probelen)) {
                return AVPROBE_SCORE_MAX;
            }
        }
    }
    // probably valid EBML header but no recognized doctype
    return AVPROBE_SCORE_EXTENSION;
}

static MXVTrack *mxv_find_track_by_num(MXVDemuxContext *mxv,
                                                 int num)
{
    MXVTrack *tracks = mxv->tracks.elem;
    int i;

    for (i = 0; i < mxv->tracks.nb_elem; i++)
        if (tracks[i].num == num)
            return &tracks[i];

    av_log(mxv->ctx, AV_LOG_ERROR, "Invalid track number %d\n", num);
    return NULL;
}

static int mxv_decrypt_buffer(uint8_t **buf, int *buf_size,
                              MXVTrack *track, MXVDemuxContext *mxv, int keyframe)
{
    MXVTrackEncoding *encodings = track->encodings.elem;
    uint8_t *data = *buf;
    int pkt_size = *buf_size;

    if (pkt_size >= 10000000U)
    {
        return AVERROR_INVALIDDATA;
    }
    switch (encodings[0].encryption.algo)
    {
        /* TODO ADD decrypt method
    case MXV_TRACK_ENCODING_ENC_DES:
        break;
    case MXV_TRACK_ENCODING_ENC_TRIPLEDES:
        break;
    case MXV_TRACK_ENCODING_ENC_TWOFISH:
        break;
    case MXV_TRACK_ENCODING_ECN_BLOWF
        break;
        */
        case MXV_TRACK_ENCODING_ENC_AES:
        {
            ff_mxv_decrypt_aes128(data, mxv->aes_key, data, pkt_size);
        }
        break;

        default:
        {

        }
        break;
    }
    return 0;
}

static int mxv_decode_buffer(uint8_t **buf, int *buf_size,
                                  MXVTrack *track)
{
    MXVTrackEncoding *encodings = track->encodings.elem;
    uint8_t *data = *buf;
    int isize = *buf_size;
    uint8_t *pkt_data = NULL;
    uint8_t av_unused *newpktdata;
    int pkt_size = isize;
    int result = 0;
    int olen;

    if (pkt_size >= 10000000U)
        return AVERROR_INVALIDDATA;

    switch (encodings[0].compression.algo) {
    case MXV_TRACK_ENCODING_COMP_HEADERSTRIP:
    {
        int header_size = encodings[0].compression.settings.size;
        uint8_t *header = encodings[0].compression.settings.data;

        if (header_size && !header) {
            av_log(NULL, AV_LOG_ERROR, "Compression size but no data in headerstrip\n");
            return -1;
        }

        if (!header_size)
            return 0;

        pkt_size = isize + header_size;
        pkt_data = av_malloc(pkt_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!pkt_data)
            return AVERROR(ENOMEM);

        memcpy(pkt_data, header, header_size);
        memcpy(pkt_data + header_size, data, isize);
        break;
    }
#if CONFIG_LZO
    case MXV_TRACK_ENCODING_COMP_LZO:
        do {
            olen       = pkt_size *= 3;
            newpktdata = av_realloc(pkt_data, pkt_size + AV_LZO_OUTPUT_PADDING
                                                       + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!newpktdata) {
                result = AVERROR(ENOMEM);
                goto failed;
            }
            pkt_data = newpktdata;
            result   = av_lzo1x_decode(pkt_data, &olen, data, &isize);
        } while (result == AV_LZO_OUTPUT_FULL && pkt_size < 10000000);
        if (result) {
            result = AVERROR_INVALIDDATA;
            goto failed;
        }
        pkt_size -= olen;
        break;
#endif
#if CONFIG_ZLIB
    case MXV_TRACK_ENCODING_COMP_ZLIB:
    {
        z_stream zstream = { 0 };
        if (inflateInit(&zstream) != Z_OK)
            return -1;
        zstream.next_in  = data;
        zstream.avail_in = isize;
        do {
            pkt_size  *= 3;
            newpktdata = av_realloc(pkt_data, pkt_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!newpktdata) {
                inflateEnd(&zstream);
                result = AVERROR(ENOMEM);
                goto failed;
            }
            pkt_data          = newpktdata;
            zstream.avail_out = pkt_size - zstream.total_out;
            zstream.next_out  = pkt_data + zstream.total_out;
            result = inflate(&zstream, Z_NO_FLUSH);
        } while (result == Z_OK && pkt_size < 10000000);
        pkt_size = zstream.total_out;
        inflateEnd(&zstream);
        if (result != Z_STREAM_END) {
            if (result == Z_MEM_ERROR)
                result = AVERROR(ENOMEM);
            else
                result = AVERROR_INVALIDDATA;
            goto failed;
        }
        break;
    }
#endif
#if CONFIG_BZLIB
    case MXV_TRACK_ENCODING_COMP_BZLIB:
    {
        bz_stream bzstream = { 0 };
        if (BZ2_bzDecompressInit(&bzstream, 0, 0) != BZ_OK)
            return -1;
        bzstream.next_in  = data;
        bzstream.avail_in = isize;
        do {
            pkt_size  *= 3;
            newpktdata = av_realloc(pkt_data, pkt_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!newpktdata) {
                BZ2_bzDecompressEnd(&bzstream);
                result = AVERROR(ENOMEM);
                goto failed;
            }
            pkt_data           = newpktdata;
            bzstream.avail_out = pkt_size - bzstream.total_out_lo32;
            bzstream.next_out  = pkt_data + bzstream.total_out_lo32;
            result = BZ2_bzDecompress(&bzstream);
        } while (result == BZ_OK && pkt_size < 10000000);
        pkt_size = bzstream.total_out_lo32;
        BZ2_bzDecompressEnd(&bzstream);
        if (result != BZ_STREAM_END) {
            if (result == BZ_MEM_ERROR)
                result = AVERROR(ENOMEM);
            else
                result = AVERROR_INVALIDDATA;
            goto failed;
        }
        break;
    }
#endif
    default:
        return AVERROR_INVALIDDATA;
    }

    memset(pkt_data + pkt_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    *buf      = pkt_data;
    *buf_size = pkt_size;
    return 0;

failed:
    av_free(pkt_data);
    return result;
}

static void mxv_convert_tag(AVFormatContext *s, EbmlList *list,
                                 AVDictionary **metadata, char *prefix)
{
    MXVTag *tags = list->elem;
    char key[1024];
    int i;

    for (i = 0; i < list->nb_elem; i++) {
        const char *lang = tags[i].lang &&
                           strcmp(tags[i].lang, "und") ? tags[i].lang : NULL;

        if (!tags[i].name) {
            av_log(s, AV_LOG_WARNING, "Skipping invalid tag with no TagName.\n");
            continue;
        }
        if (prefix)
            snprintf(key, sizeof(key), "%s/%s", prefix, tags[i].name);
        else
            av_strlcpy(key, tags[i].name, sizeof(key));
        if (tags[i].def || !lang) {
            av_dict_set(metadata, key, tags[i].string, 0);
            if (tags[i].sub.nb_elem)
                mxv_convert_tag(s, &tags[i].sub, metadata, key);
        }
        if (lang) {
            av_strlcat(key, "-", sizeof(key));
            av_strlcat(key, lang, sizeof(key));
            av_dict_set(metadata, key, tags[i].string, 0);
            if (tags[i].sub.nb_elem)
                mxv_convert_tag(s, &tags[i].sub, metadata, key);
        }
    }
    ff_metadata_conv(metadata, NULL, ff_mxv_metadata_conv);
}

static void mxv_convert_tags(AVFormatContext *s)
{
    MXVDemuxContext *mxv = s->priv_data ;
    MXVTags *tags = mxv->tags.elem;
    int i, j;

    for (i = 0; i < mxv->tags.nb_elem; i++) {
        if (tags[i].target.attachuid) {
            MXVAttachment *attachment = mxv->attachments.elem;
            int found = 0;
            for (j = 0; j < mxv->attachments.nb_elem; j++) {
                if (attachment[j].uid == tags[i].target.attachuid &&
                    attachment[j].stream) {
                    mxv_convert_tag(s, &tags[i].tag,
                                         &attachment[j].stream->metadata, NULL);
                    found = 1;
                }
            }
            if (!found) {
                av_log(NULL, AV_LOG_WARNING,
                       "The tags at index %d refer to a "
                       "non-existent attachment %"PRId64".\n",
                       i, tags[i].target.attachuid);
            }
        } else if (tags[i].target.chapteruid) {
            MXVChapter *chapter = mxv->chapters.elem;
            int found = 0;
            for (j = 0; j < mxv->chapters.nb_elem; j++) {
                if (chapter[j].uid == tags[i].target.chapteruid &&
                    chapter[j].chapter) {
                    mxv_convert_tag(s, &tags[i].tag,
                                         &chapter[j].chapter->metadata, NULL);
                    found = 1;
                }
            }
            if (!found) {
                av_log(NULL, AV_LOG_WARNING,
                       "The tags at index %d refer to a non-existent chapter "
                       "%"PRId64".\n",
                       i, tags[i].target.chapteruid);
            }
        } else if (tags[i].target.trackuid) {
            MXVTrack *track = mxv->tracks.elem;
            int found = 0;
            for (j = 0; j < mxv->tracks.nb_elem; j++) {
                if (track[j].uid == tags[i].target.trackuid &&
                    track[j].stream) {
                    mxv_convert_tag(s, &tags[i].tag,
                                         &track[j].stream->metadata, NULL);
                    found = 1;
               }
            }
            if (!found) {
                av_log(NULL, AV_LOG_WARNING,
                       "The tags at index %d refer to a non-existent track "
                       "%"PRId64".\n",
                       i, tags[i].target.trackuid);
            }
        } else {
            mxv_convert_tag(s, &tags[i].tag, &s->metadata,
                                 tags[i].target.type);
        }
    }
}

static int mxv_parse_seekhead_entry(MXVDemuxContext *mxv,
                                         int64_t pos)
{
    uint32_t saved_id  = mxv->current_id;
    int64_t before_pos = avio_tell(mxv->ctx->pb);
    int ret = 0;

    /* seek */
    if (avio_seek(mxv->ctx->pb, pos, SEEK_SET) == pos) {
        /* We don't want to lose our seekhead level, so we add
         * a dummy. This is a crude hack. */
        if (mxv->num_levels == EBML_MAX_DEPTH) {
            av_log(mxv->ctx, AV_LOG_INFO,
                   "Max EBML element depth (%d) reached, "
                   "cannot parse further.\n", EBML_MAX_DEPTH);
            ret = AVERROR_INVALIDDATA;
        } else {
            mxv->levels[mxv->num_levels] = (MXVLevel) { 0, EBML_UNKNOWN_LENGTH };
            mxv->num_levels++;
            mxv->current_id                   = 0;

            ret = ebml_parse(mxv, mxv_segment, mxv);
            if (ret == LEVEL_ENDED) {
                /* This can only happen if the seek brought us beyond EOF. */
                ret = AVERROR_EOF;
            }
        }
    }
    /* Seek back - notice that in all instances where this is used
     * it is safe to set the level to 1. */
    mxv_reset_status(mxv, saved_id, before_pos);

    return ret;
}

static void mxv_execute_seekhead(MXVDemuxContext *mxv)
{
    EbmlList *seekhead_list = &mxv->seekhead;
    int i;

    // we should not do any seeking in the streaming case
    if (!(mxv->ctx->pb->seekable & AVIO_SEEKABLE_NORMAL))
        return;

    for (i = 0; i < seekhead_list->nb_elem; i++) {
        MXVSeekhead *seekheads = seekhead_list->elem;
        uint32_t id = seekheads[i].id;
        int64_t pos = seekheads[i].pos + mxv->segment_start;

        MXVLevel1Element *elem = mxv_find_level1_elem(mxv, id);
        if (!elem || elem->parsed)
            continue;

        elem->pos = pos;

        // defer cues parsing until we actually need cue data.
        if (id == MXV_ID_CUES)
            continue;

        if (mxv_parse_seekhead_entry(mxv, pos) < 0) {
            // mark index as broken
            mxv->cues_parsing_deferred = -1;
            break;
        }

        elem->parsed = 1;
    }
}

static void mxv_add_index_entries(MXVDemuxContext *mxv)
{
    EbmlList *index_list;
    MXVIndex *index;
    uint64_t index_scale = 1;
    int i, j;

    if (mxv->ctx->flags & AVFMT_FLAG_IGNIDX)
        return;

    index_list = &mxv->index;
    index      = index_list->elem;
    if (index_list->nb_elem < 2)
        return;
    if (index[1].time > 1E14 / mxv->time_scale) {
        av_log(mxv->ctx, AV_LOG_WARNING, "Dropping apparently-broken index.\n");
        return;
    }
    for (i = 0; i < index_list->nb_elem; i++) {
        EbmlList *pos_list    = &index[i].pos;
        MXVIndexPos *pos = pos_list->elem;
        for (j = 0; j < pos_list->nb_elem; j++) {
            MXVTrack *track = mxv_find_track_by_num(mxv,
                                                              pos[j].track);
            if (track && track->stream)
                av_add_index_entry(track->stream,
                                   pos[j].pos + mxv->segment_start,
                                   index[i].time / index_scale, 0, 0,
                                   AVINDEX_KEYFRAME);
        }
    }
}

static void mxv_parse_cues(MXVDemuxContext *mxv) {
    int i;

    if (mxv->ctx->flags & AVFMT_FLAG_IGNIDX)
        return;

    for (i = 0; i < mxv->num_level1_elems; i++) {
        MXVLevel1Element *elem = &mxv->level1_elems[i];
        if (elem->id == MXV_ID_CUES && !elem->parsed) {
            if (mxv_parse_seekhead_entry(mxv, elem->pos) < 0)
                mxv->cues_parsing_deferred = -1;
            elem->parsed = 1;
            break;
        }
    }

    mxv_add_index_entries(mxv);
}

static int mxv_aac_profile(char *codec_id)
{
    static const char *const aac_profiles[] = { "MAIN", "LC", "SSR" };
    int profile;

    for (profile = 0; profile < FF_ARRAY_ELEMS(aac_profiles); profile++)
        if (strstr(codec_id, aac_profiles[profile]))
            break;
    return profile + 1;
}

static int mxv_aac_sri(int samplerate)
{
    int sri;

    for (sri = 0; sri < FF_ARRAY_ELEMS(avpriv_mpeg4audio_sample_rates); sri++)
        if (avpriv_mpeg4audio_sample_rates[sri] == samplerate)
            break;
    return sri;
}

static void mxv_metadata_creation_time(AVDictionary **metadata, int64_t date_utc)
{
    /* Convert to seconds and adjust by number of seconds between 2001-01-01 and Epoch */
    avpriv_dict_set_timestamp(metadata, "creation_time", date_utc / 1000 + 978307200000000LL);
}

static int mxv_parse_flac(AVFormatContext *s,
                               MXVTrack *track,
                               int *offset)
{
    AVStream *st = track->stream;
    uint8_t *p = track->codec_priv.data;
    int size   = track->codec_priv.size;

    if (size < 8 + FLAC_STREAMINFO_SIZE || p[4] & 0x7f) {
        av_log(s, AV_LOG_WARNING, "Invalid FLAC private data\n");
        track->codec_priv.size = 0;
        return 0;
    }
    *offset = 8;
    track->codec_priv.size = 8 + FLAC_STREAMINFO_SIZE;

    p    += track->codec_priv.size;
    size -= track->codec_priv.size;

    /* parse the remaining metadata blocks if present */
    while (size >= 4) {
        int block_last, block_type, block_size;

        flac_parse_block_header(p, &block_last, &block_type, &block_size);

        p    += 4;
        size -= 4;
        if (block_size > size)
            return 0;

        /* check for the channel mask */
        if (block_type == FLAC_METADATA_TYPE_VORBIS_COMMENT) {
            AVDictionary *dict = NULL;
            AVDictionaryEntry *chmask;

            ff_vorbis_comment(s, &dict, p, block_size, 0);
            chmask = av_dict_get(dict, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK", NULL, 0);
            if (chmask) {
                uint64_t mask = strtol(chmask->value, NULL, 0);
                if (!mask || mask & ~0x3ffffULL) {
                    av_log(s, AV_LOG_WARNING,
                           "Invalid value of WAVEFORMATEXTENSIBLE_CHANNEL_MASK\n");
                } else
                    st->codecpar->channel_layout = mask;
            }
            av_dict_free(&dict);
        }

        p    += block_size;
        size -= block_size;
    }

    return 0;
}

static int mxv_field_order(MXVDemuxContext *mxv, int64_t field_order)
{
    int major, minor, micro, bttb = 0;

    /* workaround a bug in our MXV muxer, introduced in version 57.36 alongside
     * this function, and fixed in 57.52 */
    if (mxv->muxingapp && sscanf(mxv->muxingapp, "Lavf%d.%d.%d", &major, &minor, &micro) == 3)
        bttb = (major == 57 && minor >= 36 && minor <= 51 && micro >= 100);

    switch (field_order) {
    case MXV_VIDEO_FIELDORDER_PROGRESSIVE:
        return AV_FIELD_PROGRESSIVE;
    case MXV_VIDEO_FIELDORDER_UNDETERMINED:
        return AV_FIELD_UNKNOWN;
    case MXV_VIDEO_FIELDORDER_TT:
        return AV_FIELD_TT;
    case MXV_VIDEO_FIELDORDER_BB:
        return AV_FIELD_BB;
    case MXV_VIDEO_FIELDORDER_BT:
        return bttb ? AV_FIELD_TB : AV_FIELD_BT;
    case MXV_VIDEO_FIELDORDER_TB:
        return bttb ? AV_FIELD_BT : AV_FIELD_TB;
    default:
        return AV_FIELD_UNKNOWN;
    }
}

static void mxv_stereo_mode_display_mul(int stereo_mode,
                                        int *h_width, int *h_height)
{
    switch (stereo_mode) {
        case MXV_VIDEO_STEREOMODE_TYPE_MONO:
        case MXV_VIDEO_STEREOMODE_TYPE_CHECKERBOARD_RL:
        case MXV_VIDEO_STEREOMODE_TYPE_CHECKERBOARD_LR:
        case MXV_VIDEO_STEREOMODE_TYPE_BOTH_EYES_BLOCK_RL:
        case MXV_VIDEO_STEREOMODE_TYPE_BOTH_EYES_BLOCK_LR:
            break;
        case MXV_VIDEO_STEREOMODE_TYPE_RIGHT_LEFT:
        case MXV_VIDEO_STEREOMODE_TYPE_LEFT_RIGHT:
        case MXV_VIDEO_STEREOMODE_TYPE_COL_INTERLEAVED_RL:
        case MXV_VIDEO_STEREOMODE_TYPE_COL_INTERLEAVED_LR:
            *h_width = 2;
            break;
        case MXV_VIDEO_STEREOMODE_TYPE_BOTTOM_TOP:
        case MXV_VIDEO_STEREOMODE_TYPE_TOP_BOTTOM:
        case MXV_VIDEO_STEREOMODE_TYPE_ROW_INTERLEAVED_RL:
        case MXV_VIDEO_STEREOMODE_TYPE_ROW_INTERLEAVED_LR:
            *h_height = 2;
            break;
    }
}

static int mxv_parse_video_color(AVStream *st, const MXVTrack *track) {
    const MXVTrackVideoColor *color = track->video.color.elem;
    const MXVMasteringMeta *mastering_meta;
    int has_mastering_primaries, has_mastering_luminance;

    if (!track->video.color.nb_elem)
        return 0;

    mastering_meta = &color->mastering_meta;
    // Mastering primaries are CIE 1931 coords, and must be > 0.
    has_mastering_primaries =
        mastering_meta->r_x > 0 && mastering_meta->r_y > 0 &&
        mastering_meta->g_x > 0 && mastering_meta->g_y > 0 &&
        mastering_meta->b_x > 0 && mastering_meta->b_y > 0 &&
        mastering_meta->white_x > 0 && mastering_meta->white_y > 0;
    has_mastering_luminance = mastering_meta->max_luminance > 0;

    if (color->matrix_coefficients != AVCOL_SPC_RESERVED)
        st->codecpar->color_space = color->matrix_coefficients;
    if (color->primaries != AVCOL_PRI_RESERVED &&
        color->primaries != AVCOL_PRI_RESERVED0)
        st->codecpar->color_primaries = color->primaries;
    if (color->transfer_characteristics != AVCOL_TRC_RESERVED &&
        color->transfer_characteristics != AVCOL_TRC_RESERVED0)
        st->codecpar->color_trc = color->transfer_characteristics;
    if (color->range != AVCOL_RANGE_UNSPECIFIED &&
        color->range <= AVCOL_RANGE_JPEG)
        st->codecpar->color_range = color->range;
    if (color->chroma_siting_horz != MXV_COLOUR_CHROMASITINGHORZ_UNDETERMINED &&
        color->chroma_siting_vert != MXV_COLOUR_CHROMASITINGVERT_UNDETERMINED &&
        color->chroma_siting_horz  < MXV_COLOUR_CHROMASITINGHORZ_NB &&
        color->chroma_siting_vert  < MXV_COLOUR_CHROMASITINGVERT_NB) {
        st->codecpar->chroma_location =
            avcodec_chroma_pos_to_enum((color->chroma_siting_horz - 1) << 7,
                                       (color->chroma_siting_vert - 1) << 7);
    }
    if (color->max_cll && color->max_fall) {
        size_t size = 0;
        int ret;
        AVContentLightMetadata *metadata = av_content_light_metadata_alloc(&size);
        if (!metadata)
            return AVERROR(ENOMEM);
        ret = av_stream_add_side_data(st, AV_PKT_DATA_CONTENT_LIGHT_LEVEL,
                                      (uint8_t *)metadata, size);
        if (ret < 0) {
            av_freep(&metadata);
            return ret;
        }
        metadata->MaxCLL  = color->max_cll;
        metadata->MaxFALL = color->max_fall;
    }

    if (has_mastering_primaries || has_mastering_luminance) {
        // Use similar rationals as other standards.
        const int chroma_den = 50000;
        const int luma_den = 10000;
        AVMasteringDisplayMetadata *metadata =
            (AVMasteringDisplayMetadata*) av_stream_new_side_data(
                st, AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
                sizeof(AVMasteringDisplayMetadata));
        if (!metadata) {
            return AVERROR(ENOMEM);
        }
        memset(metadata, 0, sizeof(AVMasteringDisplayMetadata));
        if (has_mastering_primaries) {
            metadata->display_primaries[0][0] = av_make_q(
                round(mastering_meta->r_x * chroma_den), chroma_den);
            metadata->display_primaries[0][1] = av_make_q(
                round(mastering_meta->r_y * chroma_den), chroma_den);
            metadata->display_primaries[1][0] = av_make_q(
                round(mastering_meta->g_x * chroma_den), chroma_den);
            metadata->display_primaries[1][1] = av_make_q(
                round(mastering_meta->g_y * chroma_den), chroma_den);
            metadata->display_primaries[2][0] = av_make_q(
                round(mastering_meta->b_x * chroma_den), chroma_den);
            metadata->display_primaries[2][1] = av_make_q(
                round(mastering_meta->b_y * chroma_den), chroma_den);
            metadata->white_point[0] = av_make_q(
                round(mastering_meta->white_x * chroma_den), chroma_den);
            metadata->white_point[1] = av_make_q(
                round(mastering_meta->white_y * chroma_den), chroma_den);
            metadata->has_primaries = 1;
        }
        if (has_mastering_luminance) {
            metadata->max_luminance = av_make_q(
                round(mastering_meta->max_luminance * luma_den), luma_den);
            metadata->min_luminance = av_make_q(
                round(mastering_meta->min_luminance * luma_den), luma_den);
            metadata->has_luminance = 1;
        }
    }
    return 0;
}

static int mxv_parse_video_projection(AVStream *st, const MXVTrack *track) {
    AVSphericalMapping *spherical;
    enum AVSphericalProjection projection;
    size_t spherical_size;
    uint32_t l = 0, t = 0, r = 0, b = 0;
    uint32_t padding = 0;
    int ret;
    GetByteContext gb;

    bytestream2_init(&gb, track->video.projection.private.data,
                     track->video.projection.private.size);

    if (bytestream2_get_byte(&gb) != 0) {
        av_log(NULL, AV_LOG_WARNING, "Unknown spherical metadata\n");
        return 0;
    }

    bytestream2_skip(&gb, 3); // flags

    switch (track->video.projection.type) {
    case MXV_VIDEO_PROJECTION_TYPE_EQUIRECTANGULAR:
        if (track->video.projection.private.size == 20) {
            t = bytestream2_get_be32(&gb);
            b = bytestream2_get_be32(&gb);
            l = bytestream2_get_be32(&gb);
            r = bytestream2_get_be32(&gb);

            if (b >= UINT_MAX - t || r >= UINT_MAX - l) {
                av_log(NULL, AV_LOG_ERROR,
                       "Invalid bounding rectangle coordinates "
                       "%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32"\n",
                       l, t, r, b);
                return AVERROR_INVALIDDATA;
            }
        } else if (track->video.projection.private.size != 0) {
            av_log(NULL, AV_LOG_ERROR, "Unknown spherical metadata\n");
            return AVERROR_INVALIDDATA;
        }

        if (l || t || r || b)
            projection = AV_SPHERICAL_EQUIRECTANGULAR_TILE;
        else
            projection = AV_SPHERICAL_EQUIRECTANGULAR;
        break;
    case MXV_VIDEO_PROJECTION_TYPE_CUBEMAP:
        if (track->video.projection.private.size < 4) {
            av_log(NULL, AV_LOG_ERROR, "Missing projection private properties\n");
            return AVERROR_INVALIDDATA;
        } else if (track->video.projection.private.size == 12) {
            uint32_t layout = bytestream2_get_be32(&gb);
            if (layout) {
                av_log(NULL, AV_LOG_WARNING,
                       "Unknown spherical cubemap layout %"PRIu32"\n", layout);
                return 0;
            }
            projection = AV_SPHERICAL_CUBEMAP;
            padding = bytestream2_get_be32(&gb);
        } else {
            av_log(NULL, AV_LOG_ERROR, "Unknown spherical metadata\n");
            return AVERROR_INVALIDDATA;
        }
        break;
    case MXV_VIDEO_PROJECTION_TYPE_RECTANGULAR:
        /* No Spherical metadata */
        return 0;
    default:
        av_log(NULL, AV_LOG_WARNING,
               "Unknown spherical metadata type %"PRIu64"\n",
               track->video.projection.type);
        return 0;
    }

    spherical = av_spherical_alloc(&spherical_size);
    if (!spherical)
        return AVERROR(ENOMEM);

    spherical->projection = projection;

    spherical->yaw   = (int32_t) (track->video.projection.yaw   * (1 << 16));
    spherical->pitch = (int32_t) (track->video.projection.pitch * (1 << 16));
    spherical->roll  = (int32_t) (track->video.projection.roll  * (1 << 16));

    spherical->padding = padding;

    spherical->bound_left   = l;
    spherical->bound_top    = t;
    spherical->bound_right  = r;
    spherical->bound_bottom = b;

    ret = av_stream_add_side_data(st, AV_PKT_DATA_SPHERICAL, (uint8_t *)spherical,
                                  spherical_size);
    if (ret < 0) {
        av_freep(&spherical);
        return ret;
    }

    return 0;
}

static int get_qt_codec(MXVTrack *track, uint32_t *fourcc, enum AVCodecID *codec_id)
{
    const AVCodecTag *codec_tags;

    codec_tags = track->type == MXV_TRACK_TYPE_VIDEO ?
            ff_codec_movvideo_tags : ff_codec_movaudio_tags;

    /* Normalize noncompliant private data that starts with the fourcc
     * by expanding/shifting the data by 4 bytes and storing the data
     * size at the start. */
    if (ff_codec_get_id(codec_tags, AV_RL32(track->codec_priv.data))) {
        int ret = av_buffer_realloc(&track->codec_priv.buf,
                                    track->codec_priv.size + 4 + AV_INPUT_BUFFER_PADDING_SIZE);
        if (ret < 0)
            return ret;

        track->codec_priv.data = track->codec_priv.buf->data;
        memmove(track->codec_priv.data + 4, track->codec_priv.data, track->codec_priv.size);
        track->codec_priv.size += 4;
        AV_WB32(track->codec_priv.data, track->codec_priv.size);
    }

    *fourcc = AV_RL32(track->codec_priv.data + 4);
    *codec_id = ff_codec_get_id(codec_tags, *fourcc);

    return 0;
}

static int mxv_parse_tracks(AVFormatContext *s)
{
    MXVDemuxContext *mxv = s->priv_data ;
    MXVTrack *tracks = mxv->tracks.elem;
    AVStream *st;
    int i, j, ret;
    int k;

    for (i = 0; i < mxv->tracks.nb_elem; i++) {
        MXVTrack *track = &tracks[i];
        enum AVCodecID codec_id = AV_CODEC_ID_NONE;
        EbmlList *encodings_list = &track->encodings;
        MXVTrackEncoding *encodings = encodings_list->elem;
        uint8_t *extradata = NULL;
        int extradata_size = 0;
        int extradata_offset = 0;
        uint32_t fourcc = 0;
        AVIOContext b;
        char* key_id_base64 = NULL;
        int bit_depth = -1;

        /* Apply some sanity checks. */
        if (track->type != MXV_TRACK_TYPE_VIDEO &&
            track->type != MXV_TRACK_TYPE_AUDIO &&
            track->type != MXV_TRACK_TYPE_SUBTITLE &&
            track->type != MXV_TRACK_TYPE_METADATA) {
            av_log(mxv->ctx, AV_LOG_INFO,
                   "Unknown or unsupported track type %"PRIu64"\n",
                   track->type);
            continue;
        }
        if (!track->codec_id)
            continue;

        if (track->audio.samplerate < 0 || track->audio.samplerate > INT_MAX ||
            isnan(track->audio.samplerate)) {
            av_log(mxv->ctx, AV_LOG_WARNING,
                   "Invalid sample rate %f, defaulting to 8000 instead.\n",
                   track->audio.samplerate);
            track->audio.samplerate = 8000;
        }

        if (track->type == MXV_TRACK_TYPE_VIDEO) {
            if (!track->default_duration && track->video.frame_rate > 0) {
                double default_duration = 1000000000 / track->video.frame_rate;
                if (default_duration > UINT64_MAX || default_duration < 0) {
                    av_log(mxv->ctx, AV_LOG_WARNING,
                         "Invalid frame rate %e. Cannot calculate default duration.\n",
                         track->video.frame_rate);
                } else {
                    track->default_duration = default_duration;
                }
            }
            if (track->video.display_width == -1)
                track->video.display_width = track->video.pixel_width;
            if (track->video.display_height == -1)
                track->video.display_height = track->video.pixel_height;
            if (track->video.color_space.size == 4)
                fourcc = AV_RL32(track->video.color_space.data);
        } else if (track->type == MXV_TRACK_TYPE_AUDIO) {
            if (!track->audio.out_samplerate)
                track->audio.out_samplerate = track->audio.samplerate;
        }
        if (encodings_list->nb_elem > 1) {
            av_log(mxv->ctx, AV_LOG_ERROR,
                   "Multiple combined encodings not supported");
        } else if (encodings_list->nb_elem == 1) {
            if (encodings[0].type == MXV_TRACK_ENCODING_TYPE_ENCRYPTION) {
                if (encodings[0].encryption.key_id.size > 0) {
                    /* Save the encryption key id to be stored later as a
                       metadata tag. */
//                    const int b64_size = AV_BASE64_SIZE(encodings[0].encryption.key_id.size);
//                    key_id_base64 = av_malloc(b64_size);
//                    if (key_id_base64 == NULL)
//                        return AVERROR(ENOMEM);
//                    av_base64_encode(key_id_base64, b64_size,
//                    encodings[0].encryption.key_id.data,
//                    encodings[0].encryption.key_id.size);

//                    key_id_base64 = av_malloc(encodings[0].encryption.key_id.size);
//                    key_id_base64 = memcpy(key_id_base64, encodings[0].encryption.key_id.data, encodings[0].encryption.key_id.size);
//                    if (key_id_base64 == NULL)
//                        return AVERROR(ENOMEM);

                    const int b64_size = AV_BASE64_DECODE_SIZE(encodings[0].encryption.key_id.size);
                    mxv->aes_key = av_mallocz(b64_size);
                    av_base64_decode(mxv->aes_key, (const char *)encodings[0].encryption.key_id.data, b64_size);


                } else {
//                    encodings[0].scope = 0;
//                    av_log(mxv->ctx, AV_LOG_ERROR,
//                           "Unsupported encoding type");
                }
            } else if (
#if CONFIG_ZLIB
                 encodings[0].compression.algo != MXV_TRACK_ENCODING_COMP_ZLIB  &&
#endif
#if CONFIG_BZLIB
                 encodings[0].compression.algo != MXV_TRACK_ENCODING_COMP_BZLIB &&
#endif
#if CONFIG_LZO
                 encodings[0].compression.algo != MXV_TRACK_ENCODING_COMP_LZO   &&
#endif
                 encodings[0].compression.algo != MXV_TRACK_ENCODING_COMP_HEADERSTRIP) {
                encodings[0].scope = 0;
                av_log(mxv->ctx, AV_LOG_ERROR,
                       "Unsupported encoding type");
            } else if (track->codec_priv.size && encodings[0].scope & 2) {
                uint8_t *codec_priv = track->codec_priv.data;
                int ret = mxv_decode_buffer(&track->codec_priv.data,
                                                 &track->codec_priv.size,
                                                 track);
                if (ret < 0) {
                    track->codec_priv.data = NULL;
                    track->codec_priv.size = 0;
                    av_log(mxv->ctx, AV_LOG_ERROR,
                           "Failed to decode codec private data\n");
                }

                if (codec_priv != track->codec_priv.data) {
                    av_buffer_unref(&track->codec_priv.buf);
                    if (track->codec_priv.data) {
                        track->codec_priv.buf = av_buffer_create(track->codec_priv.data,
                                                                 track->codec_priv.size + AV_INPUT_BUFFER_PADDING_SIZE,
                                                                 NULL, NULL, 0);
                        if (!track->codec_priv.buf) {
                            av_freep(&track->codec_priv.data);
                            track->codec_priv.size = 0;
                            return AVERROR(ENOMEM);
                        }
                    }
                }
            }
        }

        for (j = 0; ff_mxv_codec_tags[j].id != AV_CODEC_ID_NONE; j++) {
            if (!strncmp(ff_mxv_codec_tags[j].str, track->codec_id,
                         strlen(ff_mxv_codec_tags[j].str))) {
                codec_id = ff_mxv_codec_tags[j].id;
                break;
            }
        }

        st = track->stream = avformat_new_stream(s, NULL);
        if (!st) {
            av_free(key_id_base64);
            return AVERROR(ENOMEM);
        }

        if (key_id_base64) {
            /* export encryption key id as base64 metadata tag */
            av_dict_set(&st->metadata, "enc_key_id", key_id_base64, 0);
            av_freep(&key_id_base64);
        }

        if (!strcmp(track->codec_id, "V_MS/VFW/FOURCC") &&
             track->codec_priv.size >= 40               &&
            track->codec_priv.data) {
            track->ms_compat    = 1;
            bit_depth           = AV_RL16(track->codec_priv.data + 14);
            fourcc              = AV_RL32(track->codec_priv.data + 16);
            codec_id            = ff_codec_get_id(ff_codec_bmp_tags,
                                                  fourcc);
            if (!codec_id)
                codec_id        = ff_codec_get_id(ff_codec_movvideo_tags,
                                                  fourcc);
            extradata_offset    = 40;
        } else if (!strcmp(track->codec_id, "A_MS/ACM") &&
                   track->codec_priv.size >= 14         &&
                   track->codec_priv.data) {
            int ret;
            ffio_init_context(&b, track->codec_priv.data,
                              track->codec_priv.size,
                              0, NULL, NULL, NULL, NULL);
            ret = ff_get_wav_header(s, &b, st->codecpar, track->codec_priv.size, 0);
            if (ret < 0)
                return ret;
            codec_id         = st->codecpar->codec_id;
            fourcc           = st->codecpar->codec_tag;
            extradata_offset = FFMIN(track->codec_priv.size, 18);
        } else if (!strcmp(track->codec_id, "A_QUICKTIME")
                   /* Normally 36, but allow noncompliant private data */
                   && (track->codec_priv.size >= 32)
                   && (track->codec_priv.data)) {
            uint16_t sample_size;
            int ret = get_qt_codec(track, &fourcc, &codec_id);
            if (ret < 0)
                return ret;
            sample_size = AV_RB16(track->codec_priv.data + 26);
            if (fourcc == 0) {
                if (sample_size == 8) {
                    fourcc = MKTAG('r','a','w',' ');
                    codec_id = ff_codec_get_id(ff_codec_movaudio_tags, fourcc);
                } else if (sample_size == 16) {
                    fourcc = MKTAG('t','w','o','s');
                    codec_id = ff_codec_get_id(ff_codec_movaudio_tags, fourcc);
                }
            }
            if ((fourcc == MKTAG('t','w','o','s') ||
                    fourcc == MKTAG('s','o','w','t')) &&
                    sample_size == 8)
                codec_id = AV_CODEC_ID_PCM_S8;
        } else if (!strcmp(track->codec_id, "V_QUICKTIME") &&
                   (track->codec_priv.size >= 21)          &&
                   (track->codec_priv.data)) {
            int ret = get_qt_codec(track, &fourcc, &codec_id);
            if (ret < 0)
                return ret;
            if (codec_id == AV_CODEC_ID_NONE && AV_RL32(track->codec_priv.data+4) == AV_RL32("SMI ")) {
                fourcc = MKTAG('S','V','Q','3');
                codec_id = ff_codec_get_id(ff_codec_movvideo_tags, fourcc);
            }
            if (codec_id == AV_CODEC_ID_NONE)
                av_log(mxv->ctx, AV_LOG_ERROR,
                       "mov FourCC not found %s.\n", av_fourcc2str(fourcc));
            if (track->codec_priv.size >= 86) {
                bit_depth = AV_RB16(track->codec_priv.data + 82);
                ffio_init_context(&b, track->codec_priv.data,
                                  track->codec_priv.size,
                                  0, NULL, NULL, NULL, NULL);
                if (ff_get_qtpalette(codec_id, &b, track->palette)) {
                    bit_depth &= 0x1F;
                    track->has_palette = 1;
                }
            }
        } else if (codec_id == AV_CODEC_ID_PCM_S16BE) {
            switch (track->audio.bitdepth) {
            case  8:
                codec_id = AV_CODEC_ID_PCM_U8;
                break;
            case 24:
                codec_id = AV_CODEC_ID_PCM_S24BE;
                break;
            case 32:
                codec_id = AV_CODEC_ID_PCM_S32BE;
                break;
            }
        } else if (codec_id == AV_CODEC_ID_PCM_S16LE) {
            switch (track->audio.bitdepth) {
            case  8:
                codec_id = AV_CODEC_ID_PCM_U8;
                break;
            case 24:
                codec_id = AV_CODEC_ID_PCM_S24LE;
                break;
            case 32:
                codec_id = AV_CODEC_ID_PCM_S32LE;
                break;
            }
        } else if (codec_id == AV_CODEC_ID_PCM_F32LE &&
                   track->audio.bitdepth == 64) {
            codec_id = AV_CODEC_ID_PCM_F64LE;
        } else if (codec_id == AV_CODEC_ID_AAC && !track->codec_priv.size) {
            int profile = mxv_aac_profile(track->codec_id);
            int sri     = mxv_aac_sri(track->audio.samplerate);
            extradata   = av_mallocz(5 + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!extradata)
                return AVERROR(ENOMEM);
            extradata[0] = (profile << 3) | ((sri & 0x0E) >> 1);
            extradata[1] = ((sri & 0x01) << 7) | (track->audio.channels << 3);
            if (strstr(track->codec_id, "SBR")) {
                sri            = mxv_aac_sri(track->audio.out_samplerate);
                extradata[2]   = 0x56;
                extradata[3]   = 0xE5;
                extradata[4]   = 0x80 | (sri << 3);
                extradata_size = 5;
            } else
                extradata_size = 2;
        } else if (codec_id == AV_CODEC_ID_ALAC && track->codec_priv.size && track->codec_priv.size < INT_MAX - 12 - AV_INPUT_BUFFER_PADDING_SIZE) {
            /* Only ALAC's magic cookie is stored in MXV's track headers.
             * Create the "atom size", "tag", and "tag version" fields the
             * decoder expects manually. */
            extradata_size = 12 + track->codec_priv.size;
            extradata      = av_mallocz(extradata_size +
                                        AV_INPUT_BUFFER_PADDING_SIZE);
            if (!extradata)
                return AVERROR(ENOMEM);
            AV_WB32(extradata, extradata_size);
            memcpy(&extradata[4], "alac", 4);
            AV_WB32(&extradata[8], 0);
            memcpy(&extradata[12], track->codec_priv.data,
                   track->codec_priv.size);
        } else if (codec_id == AV_CODEC_ID_TTA) {
            extradata_size = 30;
            extradata      = av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!extradata)
                return AVERROR(ENOMEM);
            ffio_init_context(&b, extradata, extradata_size, 1,
                              NULL, NULL, NULL, NULL);
            avio_write(&b, "TTA1", 4);
            avio_wl16(&b, 1);
            if (track->audio.channels > UINT16_MAX ||
                track->audio.bitdepth > UINT16_MAX) {
                av_log(mxv->ctx, AV_LOG_WARNING,
                       "Too large audio channel number %"PRIu64
                       " or bitdepth %"PRIu64". Skipping track.\n",
                       track->audio.channels, track->audio.bitdepth);
                av_freep(&extradata);
                if (mxv->ctx->error_recognition & AV_EF_EXPLODE)
                    return AVERROR_INVALIDDATA;
                else
                    continue;
            }
            avio_wl16(&b, track->audio.channels);
            avio_wl16(&b, track->audio.bitdepth);
            if (track->audio.out_samplerate < 0 || track->audio.out_samplerate > INT_MAX)
                return AVERROR_INVALIDDATA;
            avio_wl32(&b, track->audio.out_samplerate);
            avio_wl32(&b, av_rescale((mxv->duration * mxv->time_scale),
                                     track->audio.out_samplerate,
                                     AV_TIME_BASE * 1000));
        } else if (codec_id == AV_CODEC_ID_RV10 ||
                   codec_id == AV_CODEC_ID_RV20 ||
                   codec_id == AV_CODEC_ID_RV30 ||
                   codec_id == AV_CODEC_ID_RV40) {
            extradata_offset = 26;
        } else if (codec_id == AV_CODEC_ID_RA_144) {
            track->audio.out_samplerate = 8000;
            track->audio.channels       = 1;
        } else if ((codec_id == AV_CODEC_ID_RA_288 ||
                    codec_id == AV_CODEC_ID_COOK   ||
                    codec_id == AV_CODEC_ID_ATRAC3 ||
                    codec_id == AV_CODEC_ID_SIPR)
                      && track->codec_priv.data) {
            int flavor;

            ffio_init_context(&b, track->codec_priv.data,
                              track->codec_priv.size,
                              0, NULL, NULL, NULL, NULL);
            avio_skip(&b, 22);
            flavor                       = avio_rb16(&b);
            track->audio.coded_framesize = avio_rb32(&b);
            avio_skip(&b, 12);
            track->audio.sub_packet_h    = avio_rb16(&b);
            track->audio.frame_size      = avio_rb16(&b);
            track->audio.sub_packet_size = avio_rb16(&b);
            if (flavor                        < 0 ||
                track->audio.coded_framesize <= 0 ||
                track->audio.sub_packet_h    <= 0 ||
                track->audio.frame_size      <= 0 ||
                track->audio.sub_packet_size <= 0 && codec_id != AV_CODEC_ID_SIPR)
                return AVERROR_INVALIDDATA;
            track->audio.buf = av_malloc_array(track->audio.sub_packet_h,
                                               track->audio.frame_size);
            if (!track->audio.buf)
                return AVERROR(ENOMEM);
            if (codec_id == AV_CODEC_ID_RA_288) {
                st->codecpar->block_align = track->audio.coded_framesize;
                track->codec_priv.size = 0;
            } else {
                if (codec_id == AV_CODEC_ID_SIPR && flavor < 4) {
                    static const int sipr_bit_rate[4] = { 6504, 8496, 5000, 16000 };
                    track->audio.sub_packet_size = ff_sipr_subpk_size[flavor];
                    st->codecpar->bit_rate          = sipr_bit_rate[flavor];
                }
                st->codecpar->block_align = track->audio.sub_packet_size;
                extradata_offset       = 78;
            }
        } else if (codec_id == AV_CODEC_ID_FLAC && track->codec_priv.size) {
            ret = mxv_parse_flac(s, track, &extradata_offset);
            if (ret < 0)
                return ret;
        } else if (codec_id == AV_CODEC_ID_PRORES && track->codec_priv.size == 4) {
            fourcc = AV_RL32(track->codec_priv.data);
        } else if (codec_id == AV_CODEC_ID_VP9 && track->codec_priv.size) {
            /* we don't need any value stored in CodecPrivate.
               make sure that it's not exported as extradata. */
            track->codec_priv.size = 0;
        } else if (codec_id == AV_CODEC_ID_AV1 && track->codec_priv.size) {
            /* For now, propagate only the OBUs, if any. Once libavcodec is
               updated to handle isobmff style extradata this can be removed. */
            extradata_offset = 4;
        }
        track->codec_priv.size -= extradata_offset;

        if (codec_id == AV_CODEC_ID_NONE)
            av_log(mxv->ctx, AV_LOG_INFO,
                   "Unknown/unsupported AVCodecID %s.\n", track->codec_id);

        if (track->time_scale < 0.01)
            track->time_scale = 1.0;
        avpriv_set_pts_info(st, 64, mxv->time_scale * track->time_scale,
                            1000 * 1000 * 1000);    /* 64 bit pts in ns */

        /* convert the delay from ns to the track timebase */
        track->codec_delay_in_track_tb = av_rescale_q(track->codec_delay,
                                          (AVRational){ 1, 1000000000 },
                                          st->time_base);

        st->codecpar->codec_id = codec_id;

        if (strcmp(track->language, "und"))
            av_dict_set(&st->metadata, "language", track->language, 0);
        av_dict_set(&st->metadata, "title", track->name, 0);

        if (track->flag_default)
            st->disposition |= AV_DISPOSITION_DEFAULT;
        if (track->flag_forced)
            st->disposition |= AV_DISPOSITION_FORCED;

        if (!st->codecpar->extradata) {
            if (extradata) {
                st->codecpar->extradata      = extradata;
                st->codecpar->extradata_size = extradata_size;
            } else if (track->codec_priv.data && track->codec_priv.size > 0) {
                if (ff_alloc_extradata(st->codecpar, track->codec_priv.size))
                    return AVERROR(ENOMEM);
                memcpy(st->codecpar->extradata,
                       track->codec_priv.data + extradata_offset,
                       track->codec_priv.size);
            }
        }

        if (track->type == MXV_TRACK_TYPE_VIDEO) {
            MXVTrackPlane *planes = track->operation.combine_planes.elem;
            int display_width_mul  = 1;
            int display_height_mul = 1;

            st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            st->codecpar->codec_tag  = fourcc;
            if (bit_depth >= 0)
                st->codecpar->bits_per_coded_sample = bit_depth;
            st->codecpar->width      = track->video.pixel_width;
            st->codecpar->height     = track->video.pixel_height;

            if (track->video.interlaced == MXV_VIDEO_INTERLACE_FLAG_INTERLACED)
                st->codecpar->field_order = mxv_field_order(mxv, track->video.field_order);
            else if (track->video.interlaced == MXV_VIDEO_INTERLACE_FLAG_PROGRESSIVE)
                st->codecpar->field_order = AV_FIELD_PROGRESSIVE;

            if (track->video.stereo_mode && track->video.stereo_mode < MXV_VIDEO_STEREOMODE_TYPE_NB)
                mxv_stereo_mode_display_mul(track->video.stereo_mode, &display_width_mul, &display_height_mul);

            if (track->video.display_unit < MXV_VIDEO_DISPLAYUNIT_UNKNOWN) {
                av_reduce(&st->sample_aspect_ratio.num,
                          &st->sample_aspect_ratio.den,
                          st->codecpar->height * track->video.display_width  * display_width_mul,
                          st->codecpar->width  * track->video.display_height * display_height_mul,
                          255);
            }
            if (st->codecpar->codec_id != AV_CODEC_ID_HEVC)
                st->need_parsing = AVSTREAM_PARSE_HEADERS;

            if (track->default_duration) {
                av_reduce(&st->avg_frame_rate.num, &st->avg_frame_rate.den,
                          1000000000, track->default_duration, 30000);
#if FF_API_R_FRAME_RATE
                if (   st->avg_frame_rate.num < st->avg_frame_rate.den * 1000LL
                    && st->avg_frame_rate.num > st->avg_frame_rate.den * 5LL)
                    st->r_frame_rate = st->avg_frame_rate;
#endif
            }

            /* export stereo mode flag as metadata tag */
            if (track->video.stereo_mode && track->video.stereo_mode < MXV_VIDEO_STEREOMODE_TYPE_NB)
                av_dict_set(&st->metadata, "stereo_mode", ff_mxv_video_stereo_mode[track->video.stereo_mode], 0);

            /* export alpha mode flag as metadata tag  */
            if (track->video.alpha_mode)
                av_dict_set(&st->metadata, "alpha_mode", "1", 0);

            /* if we have virtual track, mark the real tracks */
            for (j=0; j < track->operation.combine_planes.nb_elem; j++) {
                char buf[32];
                if (planes[j].type >= MXV_VIDEO_STEREO_PLANE_COUNT)
                    continue;
                snprintf(buf, sizeof(buf), "%s_%d",
                         ff_mxv_video_stereo_plane[planes[j].type], i);
                for (k=0; k < mxv->tracks.nb_elem; k++)
                    if (planes[j].uid == tracks[k].uid && tracks[k].stream) {
                        av_dict_set(&tracks[k].stream->metadata,
                                    "stereo_mode", buf, 0);
                        break;
                    }
            }
            // add stream level stereo3d side data if it is a supported format
            if (track->video.stereo_mode < MXV_VIDEO_STEREOMODE_TYPE_NB &&
                track->video.stereo_mode != 10 && track->video.stereo_mode != 12) {
                int ret = ff_mxv_stereo3d_conv(st, track->video.stereo_mode);
                if (ret < 0)
                    return ret;
            }

            ret = mxv_parse_video_color(st, track);
            if (ret < 0)
                return ret;
            ret = mxv_parse_video_projection(st, track);
            if (ret < 0)
                return ret;
        } else if (track->type == MXV_TRACK_TYPE_AUDIO) {
            st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
            st->codecpar->codec_tag   = fourcc;
            st->codecpar->sample_rate = track->audio.out_samplerate;
            st->codecpar->channels    = track->audio.channels;
            if (!st->codecpar->bits_per_coded_sample)
                st->codecpar->bits_per_coded_sample = track->audio.bitdepth;
            if (st->codecpar->codec_id == AV_CODEC_ID_MP3 ||
                st->codecpar->codec_id == AV_CODEC_ID_MLP ||
                st->codecpar->codec_id == AV_CODEC_ID_TRUEHD)
                st->need_parsing = AVSTREAM_PARSE_FULL;
            else if (st->codecpar->codec_id != AV_CODEC_ID_AAC)
                st->need_parsing = AVSTREAM_PARSE_HEADERS;
            if (track->codec_delay > 0) {
                st->codecpar->initial_padding = av_rescale_q(track->codec_delay,
                                                             (AVRational){1, 1000000000},
                                                             (AVRational){1, st->codecpar->codec_id == AV_CODEC_ID_OPUS ?
                                                                             48000 : st->codecpar->sample_rate});
            }
            if (track->seek_preroll > 0) {
                st->codecpar->seek_preroll = av_rescale_q(track->seek_preroll,
                                                          (AVRational){1, 1000000000},
                                                          (AVRational){1, st->codecpar->sample_rate});
            }
        } else if (codec_id == AV_CODEC_ID_WEBVTT) {
            st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;

            if (!strcmp(track->codec_id, "D_WEBVTT/CAPTIONS")) {
                st->disposition |= AV_DISPOSITION_CAPTIONS;
            } else if (!strcmp(track->codec_id, "D_WEBVTT/DESCRIPTIONS")) {
                st->disposition |= AV_DISPOSITION_DESCRIPTIONS;
            } else if (!strcmp(track->codec_id, "D_WEBVTT/METADATA")) {
                st->disposition |= AV_DISPOSITION_METADATA;
            }
        } else if (track->type == MXV_TRACK_TYPE_SUBTITLE) {
            st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
        }
    }

    return 0;
}

static int mxv_read_header(AVFormatContext *s)
{
    MXVDemuxContext *mxv = s->priv_data ;
    EbmlList *attachments_list = &mxv->attachments;
    EbmlList *chapters_list    = &mxv->chapters;
    MXVAttachment *attachments;
    MXVChapter *chapters;
    uint64_t max_start = 0;
    int64_t pos;
    Ebml ebml = { 0 };
    int i, j, res;

    mxv->ctx = s;
    if (_has_pre_padding)
    {
        int pre_padding_size = mxv_prePadding_size(mxv->ctx->pb);
        avio_skip(mxv->ctx->pb, pre_padding_size);
    }
    mxv->cues_parsing_deferred = 1;

    /* First read the EBML header. */
    if (ebml_parse(mxv, ebml_syntax, &ebml) || !ebml.doctype) {
        av_log(mxv->ctx, AV_LOG_ERROR, "EBML header parsing failed\n");
        ebml_free(ebml_syntax, &ebml);
        return AVERROR_INVALIDDATA;
    }
    if (ebml.version         > EBML_VERSION      ||
        ebml.max_size        > sizeof(uint64_t)  ||
        ebml.id_length       > sizeof(uint32_t)  ||
        ebml.doctype_version > 3) {
        avpriv_report_missing_feature(mxv->ctx,
                                      "EBML version %"PRIu64", doctype %s, doc version %"PRIu64,
                                      ebml.version, ebml.doctype, ebml.doctype_version);
        ebml_free(ebml_syntax, &ebml);
        return AVERROR_PATCHWELCOME;
    } else if (ebml.doctype_version == 3) {
        av_log(mxv->ctx, AV_LOG_WARNING,
               "EBML header using unsupported features\n"
               "(EBML version %"PRIu64", doctype %s, doc version %"PRIu64")\n",
               ebml.version, ebml.doctype, ebml.doctype_version);
    }
    for (i = 0; i < FF_ARRAY_ELEMS(mxv_doctypes); i++)
        if (!strcmp(ebml.doctype, mxv_doctypes[i]))
            break;
    if (i >= FF_ARRAY_ELEMS(mxv_doctypes)) {
        av_log(s, AV_LOG_WARNING, "Unknown EBML doctype '%s'\n", ebml.doctype);
        if (mxv->ctx->error_recognition & AV_EF_EXPLODE) {
            ebml_free(ebml_syntax, &ebml);
            return AVERROR_INVALIDDATA;
        }
    }
    ebml_free(ebml_syntax, &ebml);

    /* The next thing is a segment. */
    pos = avio_tell(mxv->ctx->pb);
    res = ebml_parse(mxv, mxv_segments, mxv);
    // Try resyncing until we find an EBML_STOP type element.
    while (res != 1) {
        res = mxv_resync(mxv, pos);
        if (res < 0)
            goto fail;
        pos = avio_tell(mxv->ctx->pb);
        res = ebml_parse(mxv, mxv_segment, mxv);
    }
    /* Set data_offset as it might be needed later by seek_frame_generic. */
    if (mxv->current_id == MXV_ID_CLUSTER)
        s->internal->data_offset = avio_tell(mxv->ctx->pb) - 4;
    mxv_execute_seekhead(mxv);

    if (!mxv->time_scale)
        mxv->time_scale = 1000000;
    if (mxv->duration)
        mxv->ctx->duration = mxv->duration * mxv->time_scale *
                                  1000 / AV_TIME_BASE;
    av_dict_set(&s->metadata, "title", mxv->title, 0);
    av_dict_set(&s->metadata, "encoder", mxv->muxingapp, 0);

    if (mxv->date_utc.size == 8)
        mxv_metadata_creation_time(&s->metadata, AV_RB64(mxv->date_utc.data));

    res = mxv_parse_tracks(s);
    if (res < 0)
        goto fail;

    attachments = attachments_list->elem;
    for (j = 0; j < attachments_list->nb_elem; j++) {
        if (!(attachments[j].filename && attachments[j].mime &&
              attachments[j].bin.data && attachments[j].bin.size > 0)) {
            av_log(mxv->ctx, AV_LOG_ERROR, "incomplete attachment\n");
        } else {
            AVStream *st = avformat_new_stream(s, NULL);
            if (!st)
                break;
            av_dict_set(&st->metadata, "filename", attachments[j].filename, 0);
            av_dict_set(&st->metadata, "mimetype", attachments[j].mime, 0);
            st->codecpar->codec_id   = AV_CODEC_ID_NONE;

            for (i = 0; ff_mxv_image_mime_tags[i].id != AV_CODEC_ID_NONE; i++) {
                if (!strncmp(ff_mxv_image_mime_tags[i].str, attachments[j].mime,
                             strlen(ff_mxv_image_mime_tags[i].str))) {
                    st->codecpar->codec_id = ff_mxv_image_mime_tags[i].id;
                    break;
                }
            }

            attachments[j].stream = st;

            if (st->codecpar->codec_id != AV_CODEC_ID_NONE) {
                AVPacket *pkt = &st->attached_pic;

                st->disposition         |= AV_DISPOSITION_ATTACHED_PIC;
                st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;

                av_init_packet(pkt);
                pkt->buf = av_buffer_ref(attachments[j].bin.buf);
                if (!pkt->buf)
                    return AVERROR(ENOMEM);
                pkt->data         = attachments[j].bin.data;
                pkt->size         = attachments[j].bin.size;
                pkt->stream_index = st->index;
                pkt->flags       |= AV_PKT_FLAG_KEY;
            } else {
                st->codecpar->codec_type = AVMEDIA_TYPE_ATTACHMENT;
                if (ff_alloc_extradata(st->codecpar, attachments[j].bin.size))
                    break;
                memcpy(st->codecpar->extradata, attachments[j].bin.data,
                       attachments[j].bin.size);

                for (i = 0; ff_mxv_mime_tags[i].id != AV_CODEC_ID_NONE; i++) {
                    if (!strncmp(ff_mxv_mime_tags[i].str, attachments[j].mime,
                                strlen(ff_mxv_mime_tags[i].str))) {
                        st->codecpar->codec_id = ff_mxv_mime_tags[i].id;
                        break;
                    }
                }
            }
        }
    }

    chapters = chapters_list->elem;
    for (i = 0; i < chapters_list->nb_elem; i++)
        if (chapters[i].start != AV_NOPTS_VALUE && chapters[i].uid &&
            (max_start == 0 || chapters[i].start > max_start)) {
            chapters[i].chapter =
                avpriv_new_chapter(s, chapters[i].uid,
                                   (AVRational) { 1, 1000000000 },
                                   chapters[i].start, chapters[i].end,
                                   chapters[i].title);
            if (chapters[i].chapter) {
                av_dict_set(&chapters[i].chapter->metadata,
                            "title", chapters[i].title, 0);
            }
            max_start = chapters[i].start;
        }

    mxv_add_index_entries(mxv);

    mxv_convert_tags(s);

    return 0;
fail:
    mxv_read_close(s);
    return res;
}

/*
 * Put one packet in an application-supplied AVPacket struct.
 * Returns 0 on success or -1 on failure.
 */
static int mxv_deliver_packet(MXVDemuxContext *mxv,
                                   AVPacket *pkt)
{
    if (mxv->queue) {
        MXVTrack *tracks = mxv->tracks.elem;
        MXVTrack *track;

        ff_packet_list_get(&mxv->queue, &mxv->queue_end, pkt);
        track = &tracks[pkt->stream_index];
        if (track->has_palette) {
            uint8_t *pal = av_packet_new_side_data(pkt, AV_PKT_DATA_PALETTE, AVPALETTE_SIZE);
            if (!pal) {
                av_log(mxv->ctx, AV_LOG_ERROR, "Cannot append palette to packet\n");
            } else {
                memcpy(pal, track->palette, AVPALETTE_SIZE);
            }
            track->has_palette = 0;
        }
        return 0;
    }

    return -1;
}

/*
 * Free all packets in our internal queue.
 */
static void mxv_clear_queue(MXVDemuxContext *mxv)
{
    ff_packet_list_free(&mxv->queue, &mxv->queue_end);
}

static int mxv_parse_laces(MXVDemuxContext *mxv, uint8_t **buf,
                                int *buf_size, int type,
                                uint32_t **lace_buf, int *laces)
{
    int res = 0, n, size = *buf_size;
    uint8_t *data = *buf;
    uint32_t *lace_size;

    if (!type) {
        *laces    = 1;
        *lace_buf = av_malloc(sizeof(**lace_buf));
        if (!*lace_buf)
            return AVERROR(ENOMEM);

        *lace_buf[0] = size;
        return 0;
    }

    av_assert0(size > 0);
    *laces    = *data + 1;
    data     += 1;
    size     -= 1;
    lace_size = av_malloc_array(*laces, sizeof(*lace_size));
    if (!lace_size)
        return AVERROR(ENOMEM);

    switch (type) {
    case 0x1: /* Xiph lacing */
    {
        uint8_t temp;
        uint32_t total = 0;
        for (n = 0; res == 0 && n < *laces - 1; n++) {
            lace_size[n] = 0;

            while (1) {
                if (size <= total) {
                    res = AVERROR_INVALIDDATA;
                    break;
                }
                temp          = *data;
                total        += temp;
                lace_size[n] += temp;
                data         += 1;
                size         -= 1;
                if (temp != 0xff)
                    break;
            }
        }
        if (size <= total) {
            res = AVERROR_INVALIDDATA;
            break;
        }

        lace_size[n] = size - total;
        break;
    }

    case 0x2: /* fixed-size lacing */
        if (size % (*laces)) {
            res = AVERROR_INVALIDDATA;
            break;
        }
        for (n = 0; n < *laces; n++)
            lace_size[n] = size / *laces;
        break;

    case 0x3: /* EBML lacing */
    {
        uint64_t num;
        uint64_t total;
        n = mxv_ebmlnum_uint(mxv, data, size, &num);
        if (n < 0 || num > INT_MAX) {
            av_log(mxv->ctx, AV_LOG_INFO,
                   "EBML block data error\n");
            res = n<0 ? n : AVERROR_INVALIDDATA;
            break;
        }
        data += n;
        size -= n;
        total = lace_size[0] = num;
        for (n = 1; res == 0 && n < *laces - 1; n++) {
            int64_t snum;
            int r;
            r = mxv_ebmlnum_sint(mxv, data, size, &snum);
            if (r < 0 || lace_size[n - 1] + snum > (uint64_t)INT_MAX) {
                av_log(mxv->ctx, AV_LOG_INFO,
                       "EBML block data error\n");
                res = r<0 ? r : AVERROR_INVALIDDATA;
                break;
            }
            data        += r;
            size        -= r;
            lace_size[n] = lace_size[n - 1] + snum;
            total       += lace_size[n];
        }
        if (size <= total) {
            res = AVERROR_INVALIDDATA;
            break;
        }
        lace_size[*laces - 1] = size - total;
        break;
    }
    }

    *buf      = data;
    *lace_buf = lace_size;
    *buf_size = size;

    return res;
}

static int mxv_parse_rm_audio(MXVDemuxContext *mxv,
                                   MXVTrack *track, AVStream *st,
                                   uint8_t *data, int size, uint64_t timecode,
                                   int64_t pos)
{
    int a = st->codecpar->block_align;
    int sps = track->audio.sub_packet_size;
    int cfs = track->audio.coded_framesize;
    int h   = track->audio.sub_packet_h;
    int y   = track->audio.sub_packet_cnt;
    int w   = track->audio.frame_size;
    int x;

    if (!track->audio.pkt_cnt) {
        if (track->audio.sub_packet_cnt == 0)
            track->audio.buf_timecode = timecode;
        if (st->codecpar->codec_id == AV_CODEC_ID_RA_288) {
            if (size < cfs * h / 2) {
                av_log(mxv->ctx, AV_LOG_ERROR,
                       "Corrupt int4 RM-style audio packet size\n");
                return AVERROR_INVALIDDATA;
            }
            for (x = 0; x < h / 2; x++)
                memcpy(track->audio.buf + x * 2 * w + y * cfs,
                       data + x * cfs, cfs);
        } else if (st->codecpar->codec_id == AV_CODEC_ID_SIPR) {
            if (size < w) {
                av_log(mxv->ctx, AV_LOG_ERROR,
                       "Corrupt sipr RM-style audio packet size\n");
                return AVERROR_INVALIDDATA;
            }
            memcpy(track->audio.buf + y * w, data, w);
        } else {
            if (size < sps * w / sps || h<=0 || w%sps) {
                av_log(mxv->ctx, AV_LOG_ERROR,
                       "Corrupt generic RM-style audio packet size\n");
                return AVERROR_INVALIDDATA;
            }
            for (x = 0; x < w / sps; x++)
                memcpy(track->audio.buf +
                       sps * (h * x + ((h + 1) / 2) * (y & 1) + (y >> 1)),
                       data + x * sps, sps);
        }

        if (++track->audio.sub_packet_cnt >= h) {
            if (st->codecpar->codec_id == AV_CODEC_ID_SIPR)
                ff_rm_reorder_sipr_data(track->audio.buf, h, w);
            track->audio.sub_packet_cnt = 0;
            track->audio.pkt_cnt        = h * w / a;
        }
    }

    while (track->audio.pkt_cnt) {
        int ret;
        AVPacket pktl, *pkt = &pktl;

        ret = av_new_packet(pkt, a);
        if (ret < 0) {
            return ret;
        }
        memcpy(pkt->data,
               track->audio.buf + a * (h * w / a - track->audio.pkt_cnt--),
               a);
        pkt->pts                  = track->audio.buf_timecode;
        track->audio.buf_timecode = AV_NOPTS_VALUE;
        pkt->pos                  = pos;
        pkt->stream_index         = st->index;
        ret = ff_packet_list_put(&mxv->queue, &mxv->queue_end, pkt, 0);
        if (ret < 0) {
            av_packet_unref(pkt);
            return AVERROR(ENOMEM);
        }
    }

    return 0;
}

/* reconstruct full wavpack blocks from mangled mxv ones */
static int mxv_parse_wavpack(MXVTrack *track, uint8_t *src,
                                  uint8_t **pdst, int *size)
{
    uint8_t *dst = NULL;
    int dstlen   = 0;
    int srclen   = *size;
    uint32_t samples;
    uint16_t ver;
    int ret, offset = 0;

    if (srclen < 12 || track->stream->codecpar->extradata_size < 2)
        return AVERROR_INVALIDDATA;

    ver = AV_RL16(track->stream->codecpar->extradata);

    samples = AV_RL32(src);
    src    += 4;
    srclen -= 4;

    while (srclen >= 8) {
        int multiblock;
        uint32_t blocksize;
        uint8_t *tmp;

        uint32_t flags = AV_RL32(src);
        uint32_t crc   = AV_RL32(src + 4);
        src    += 8;
        srclen -= 8;

        multiblock = (flags & 0x1800) != 0x1800;
        if (multiblock) {
            if (srclen < 4) {
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            blocksize = AV_RL32(src);
            src      += 4;
            srclen   -= 4;
        } else
            blocksize = srclen;

        if (blocksize > srclen) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        tmp = av_realloc(dst, dstlen + blocksize + 32 + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!tmp) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        dst     = tmp;
        dstlen += blocksize + 32;

        AV_WL32(dst + offset, MKTAG('w', 'v', 'p', 'k'));   // tag
        AV_WL32(dst + offset +  4, blocksize + 24);         // blocksize - 8
        AV_WL16(dst + offset +  8, ver);                    // version
        AV_WL16(dst + offset + 10, 0);                      // track/index_no
        AV_WL32(dst + offset + 12, 0);                      // total samples
        AV_WL32(dst + offset + 16, 0);                      // block index
        AV_WL32(dst + offset + 20, samples);                // number of samples
        AV_WL32(dst + offset + 24, flags);                  // flags
        AV_WL32(dst + offset + 28, crc);                    // crc
        memcpy(dst + offset + 32, src, blocksize);          // block data

        src    += blocksize;
        srclen -= blocksize;
        offset += blocksize + 32;
    }

    memset(dst + dstlen, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    *pdst = dst;
    *size = dstlen;

    return 0;

fail:
    av_freep(&dst);
    return ret;
}

static int mxv_parse_prores(MXVTrack *track, uint8_t *src,
                                 uint8_t **pdst, int *size)
{
    uint8_t *dst = src;
    int dstlen = *size;

    if (AV_RB32(&src[4]) != MKBETAG('i', 'c', 'p', 'f')) {
        dst = av_malloc(dstlen + 8 + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!dst)
            return AVERROR(ENOMEM);

        AV_WB32(dst, dstlen);
        AV_WB32(dst + 4, MKBETAG('i', 'c', 'p', 'f'));
        memcpy(dst + 8, src, dstlen);
        memset(dst + 8 + dstlen, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        dstlen += 8;
    }

    *pdst = dst;
    *size = dstlen;

    return 0;
}

static int mxv_parse_webvtt(MXVDemuxContext *mxv,
                                 MXVTrack *track,
                                 AVStream *st,
                                 uint8_t *data, int data_len,
                                 uint64_t timecode,
                                 uint64_t duration,
                                 int64_t pos)
{
    AVPacket pktl, *pkt = &pktl;
    uint8_t *id, *settings, *text, *buf;
    int id_len, settings_len, text_len;
    uint8_t *p, *q;
    int err;

    if (data_len <= 0)
        return AVERROR_INVALIDDATA;

    p = data;
    q = data + data_len;

    id = p;
    id_len = -1;
    while (p < q) {
        if (*p == '\r' || *p == '\n') {
            id_len = p - id;
            if (*p == '\r')
                p++;
            break;
        }
        p++;
    }

    if (p >= q || *p != '\n')
        return AVERROR_INVALIDDATA;
    p++;

    settings = p;
    settings_len = -1;
    while (p < q) {
        if (*p == '\r' || *p == '\n') {
            settings_len = p - settings;
            if (*p == '\r')
                p++;
            break;
        }
        p++;
    }

    if (p >= q || *p != '\n')
        return AVERROR_INVALIDDATA;
    p++;

    text = p;
    text_len = q - p;
    while (text_len > 0) {
        const int len = text_len - 1;
        const uint8_t c = p[len];
        if (c != '\r' && c != '\n')
            break;
        text_len = len;
    }

    if (text_len <= 0)
        return AVERROR_INVALIDDATA;

    err = av_new_packet(pkt, text_len);
    if (err < 0) {
        return err;
    }

    memcpy(pkt->data, text, text_len);

    if (id_len > 0) {
        buf = av_packet_new_side_data(pkt,
                                      AV_PKT_DATA_WEBVTT_IDENTIFIER,
                                      id_len);
        if (!buf) {
            av_packet_unref(pkt);
            return AVERROR(ENOMEM);
        }
        memcpy(buf, id, id_len);
    }

    if (settings_len > 0) {
        buf = av_packet_new_side_data(pkt,
                                      AV_PKT_DATA_WEBVTT_SETTINGS,
                                      settings_len);
        if (!buf) {
            av_packet_unref(pkt);
            return AVERROR(ENOMEM);
        }
        memcpy(buf, settings, settings_len);
    }

    // Do we need this for subtitles?
    // pkt->flags = AV_PKT_FLAG_KEY;

    pkt->stream_index = st->index;
    pkt->pts = timecode;

    // Do we need this for subtitles?
    // pkt->dts = timecode;

    pkt->duration = duration;
    pkt->pos = pos;

    err = ff_packet_list_put(&mxv->queue, &mxv->queue_end, pkt, 0);
    if (err < 0) {
        av_packet_unref(pkt);
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int mxv_parse_frame(MXVDemuxContext *mxv,
                                MXVTrack *track, AVStream *st,
                                AVBufferRef *buf, uint8_t *data, int pkt_size,
                                uint64_t timecode, uint64_t lace_duration,
                                int64_t pos, int is_keyframe,
                                uint8_t *additional, uint64_t additional_id, int additional_size,
                                int64_t discard_padding)
{
    MXVTrackEncoding *encodings = track->encodings.elem;
    uint8_t *pkt_data = data;
    int res;
    AVPacket pktl, *pkt = &pktl;


    if (encodings && encodings->type == MXV_TRACK_ENCODING_TYPE_COMPRESSION && encodings->scope & 1) {
        res = mxv_decode_buffer(&pkt_data, &pkt_size, track);
        if (res < 0)
            return res;
    }

    if (encodings && encodings->type == MXV_TRACK_ENCODING_TYPE_ENCRYPTION && encodings->scope & 1) {
        res = mxv_decrypt_buffer(&pkt_data, &pkt_size, track, mxv, is_keyframe);
        if (res < 0)
            return res;
    }

    if (st->codecpar->codec_id == AV_CODEC_ID_WAVPACK) {
        uint8_t *wv_data;
        res = mxv_parse_wavpack(track, pkt_data, &wv_data, &pkt_size);
        if (res < 0) {
            av_log(mxv->ctx, AV_LOG_ERROR,
                   "Error parsing a wavpack block.\n");
            goto fail;
        }
        if (pkt_data != data)
            av_freep(&pkt_data);
        pkt_data = wv_data;
    }

    if (st->codecpar->codec_id == AV_CODEC_ID_PRORES) {
        uint8_t *pr_data;
        res = mxv_parse_prores(track, pkt_data, &pr_data, &pkt_size);
        if (res < 0) {
            av_log(mxv->ctx, AV_LOG_ERROR,
                   "Error parsing a prores block.\n");
            goto fail;
        }
        if (pkt_data != data)
            av_freep(&pkt_data);
        pkt_data = pr_data;
    }

    av_init_packet(pkt);
    if (pkt_data != data)
        pkt->buf = av_buffer_create(pkt_data, pkt_size + AV_INPUT_BUFFER_PADDING_SIZE,
                                    NULL, NULL, 0);
    else
        pkt->buf = av_buffer_ref(buf);

    if (!pkt->buf) {
        res = AVERROR(ENOMEM);
        goto fail;
    }

    pkt->data         = pkt_data;
    pkt->size         = pkt_size;
    pkt->flags        = is_keyframe;
    pkt->stream_index = st->index;

    if (additional_size > 0) {
        uint8_t *side_data = av_packet_new_side_data(pkt,
                                                     AV_PKT_DATA_MXV_BLOCKADDITIONAL,
                                                     additional_size + 8);
        if (!side_data) {
            av_packet_unref(pkt);
            return AVERROR(ENOMEM);
        }
        AV_WB64(side_data, additional_id);
        memcpy(side_data + 8, additional, additional_size);
    }

    if (discard_padding) {
        uint8_t *side_data = av_packet_new_side_data(pkt,
                                                     AV_PKT_DATA_SKIP_SAMPLES,
                                                     10);
        if (!side_data) {
            av_packet_unref(pkt);
            return AVERROR(ENOMEM);
        }
        discard_padding = av_rescale_q(discard_padding,
                                            (AVRational){1, 1000000000},
                                            (AVRational){1, st->codecpar->sample_rate});
        if (discard_padding > 0) {
            AV_WL32(side_data + 4, discard_padding);
        } else {
            AV_WL32(side_data, -discard_padding);
        }
    }

    if (track->ms_compat)
        pkt->dts = timecode;
    else
        pkt->pts = timecode;
    pkt->pos = pos;
    pkt->duration = lace_duration;

#if FF_API_CONVERGENCE_DURATION
FF_DISABLE_DEPRECATION_WARNINGS
    if (st->codecpar->codec_id == AV_CODEC_ID_SUBRIP) {
        pkt->convergence_duration = lace_duration;
    }
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    res = ff_packet_list_put(&mxv->queue, &mxv->queue_end, pkt, 0);
    if (res < 0) {
        av_packet_unref(pkt);
        return AVERROR(ENOMEM);
    }

    return 0;

fail:
    if (pkt_data != data)
        av_freep(&pkt_data);
    return res;
}

static int mxv_parse_block(MXVDemuxContext *mxv, AVBufferRef *buf, uint8_t *data,
                                int size, int64_t pos, uint64_t cluster_time,
                                uint64_t block_duration, int is_keyframe,
                                uint8_t *additional, uint64_t additional_id, int additional_size,
                                int64_t cluster_pos, int64_t discard_padding)
{
    uint64_t timecode = AV_NOPTS_VALUE;
    MXVTrack *track;
    int res = 0;
    AVStream *st;
    int16_t block_time;
    uint32_t *lace_size = NULL;
    int n, flags, laces = 0;
    uint64_t num;
    int trust_default_duration = 1;

    if ((n = mxv_ebmlnum_uint(mxv, data, size, &num)) < 0) {
        return n;
    }
    data += n;
    size -= n;

    track = mxv_find_track_by_num(mxv, num);
    if (!track || !track->stream) {
        av_log(mxv->ctx, AV_LOG_INFO,
               "Invalid stream %"PRIu64"\n", num);
        return AVERROR_INVALIDDATA;
    } else if (size <= 3)
        return 0;
    st = track->stream;
    if (st->discard >= AVDISCARD_ALL)
        return res;
    av_assert1(block_duration != AV_NOPTS_VALUE);

    block_time = sign_extend(AV_RB16(data), 16);
    data      += 2;
    flags      = *data++;
    size      -= 3;
    if (is_keyframe == -1)
        is_keyframe = flags & 0x80 ? AV_PKT_FLAG_KEY : 0;

    if (cluster_time != (uint64_t) -1 &&
        (block_time >= 0 || cluster_time >= -block_time)) {
        timecode = cluster_time + block_time - track->codec_delay_in_track_tb;
        if (track->type == MXV_TRACK_TYPE_SUBTITLE &&
            timecode < track->end_timecode)
            is_keyframe = 0;  /* overlapping subtitles are not key frame */
        if (is_keyframe) {
            ff_reduce_index(mxv->ctx, st->index);
            av_add_index_entry(st, cluster_pos, timecode, 0, 0,
                               AVINDEX_KEYFRAME);
        }
    }

    if (mxv->skip_to_keyframe &&
        track->type != MXV_TRACK_TYPE_SUBTITLE) {
        // Compare signed timecodes. Timecode may be negative due to codec delay
        // offset. We don't support timestamps greater than int64_t anyway - see
        // AVPacket's pts.
        if ((int64_t)timecode < (int64_t)mxv->skip_to_timecode)
            return res;
        if (is_keyframe)
            mxv->skip_to_keyframe = 0;
        else if (!st->skip_to_keyframe) {
            av_log(mxv->ctx, AV_LOG_ERROR, "File is broken, keyframes not correctly marked!\n");
            mxv->skip_to_keyframe = 0;
        }
    }

    res = mxv_parse_laces(mxv, &data, &size, (flags & 0x06) >> 1,
                               &lace_size, &laces);

    if (res)
        goto end;

    if (track->audio.samplerate == 8000) {
        // If this is needed for more codecs, then add them here
        if (st->codecpar->codec_id == AV_CODEC_ID_AC3) {
            if (track->audio.samplerate != st->codecpar->sample_rate || !st->codecpar->frame_size)
                trust_default_duration = 0;
        }
    }

    if (!block_duration && trust_default_duration)
        block_duration = track->default_duration * laces / mxv->time_scale;

    if (cluster_time != (uint64_t)-1 && (block_time >= 0 || cluster_time >= -block_time))
        track->end_timecode =
            FFMAX(track->end_timecode, timecode + block_duration);

    for (n = 0; n < laces; n++) {
        int64_t lace_duration = block_duration*(n+1) / laces - block_duration*n / laces;

        if (lace_size[n] > size) {
            av_log(mxv->ctx, AV_LOG_ERROR, "Invalid packet size\n");
            break;
        }

        if ((st->codecpar->codec_id == AV_CODEC_ID_RA_288 ||
             st->codecpar->codec_id == AV_CODEC_ID_COOK   ||
             st->codecpar->codec_id == AV_CODEC_ID_SIPR   ||
             st->codecpar->codec_id == AV_CODEC_ID_ATRAC3) &&
            st->codecpar->block_align && track->audio.sub_packet_size) {
            res = mxv_parse_rm_audio(mxv, track, st, data,
                                          lace_size[n],
                                          timecode, pos);
            if (res)
                goto end;

        } else if (st->codecpar->codec_id == AV_CODEC_ID_WEBVTT) {
            res = mxv_parse_webvtt(mxv, track, st,
                                        data, lace_size[n],
                                        timecode, lace_duration,
                                        pos);
            if (res)
                goto end;
        } else {
            res = mxv_parse_frame(mxv, track, st, buf, data, lace_size[n],
                                       timecode, lace_duration, pos,
                                       !n ? is_keyframe : 0,
                                       additional, additional_id, additional_size,
                                       discard_padding);
            if (res)
                goto end;
        }

        if (timecode != AV_NOPTS_VALUE)
            timecode = lace_duration ? timecode + lace_duration : AV_NOPTS_VALUE;
        data += lace_size[n];
        size -= lace_size[n];
    }

end:
    av_free(lace_size);
    return res;
}

static int mxv_parse_cluster(MXVDemuxContext *mxv)
{
    MXVCluster *cluster = &mxv->current_cluster;
    MXVBlock     *block = &cluster->block;
    int res;

    av_assert0(mxv->num_levels <= 2);

    if (mxv->num_levels == 1) {
        res = ebml_parse(mxv, mxv_segment, NULL);

        if (res == 1) {
            /* Found a cluster: subtract the size of the ID already read. */
            cluster->pos = avio_tell(mxv->ctx->pb) - 4;

            res = ebml_parse(mxv, mxv_cluster_enter, cluster);
            if (res < 0)
                return res;
        }
    }

    if (mxv->num_levels == 2) {
        /* We are inside a cluster. */
        res = ebml_parse(mxv, mxv_cluster_parsing, cluster);

        if (res >= 0 && block->bin.size > 0) {
            int is_keyframe = block->non_simple ? block->reference == INT64_MIN : -1;
            uint8_t* additional = block->additional.size > 0 ?
                                    block->additional.data : NULL;

            res = mxv_parse_block(mxv, block->bin.buf, block->bin.data,
                                       block->bin.size, block->bin.pos,
                                       cluster->timecode, block->duration,
                                       is_keyframe, additional, block->additional_id,
                                       block->additional.size, cluster->pos,
                                       block->discard_padding);
        }

        ebml_free(mxv_blockgroup, block);
        memset(block, 0, sizeof(*block));
    } else if (!mxv->num_levels) {
        if (!avio_feof(mxv->ctx->pb)) {
            avio_r8(mxv->ctx->pb);
            if (!avio_feof(mxv->ctx->pb)) {
                av_log(mxv->ctx, AV_LOG_WARNING, "File extends beyond "
                       "end of segment.\n");
                return AVERROR_INVALIDDATA;
            }
        }
        mxv->done = 1;
        return AVERROR_EOF;
    }

    return res;
}

static int mxv_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MXVDemuxContext *mxv = s->priv_data ;
    int ret = 0;

    if (mxv->resync_pos == -1) {
        // This can only happen if generic seeking has been used.
        mxv->resync_pos = avio_tell(s->pb);
    }

    while (mxv_deliver_packet(mxv, pkt)) {
        if (mxv->done)
            return (ret < 0) ? ret : AVERROR_EOF;
        if (mxv_parse_cluster(mxv) < 0 && !mxv->done)
            ret = mxv_resync(mxv, mxv->resync_pos);
    }

    return 0;
}

static int mxv_read_seek(AVFormatContext *s, int stream_index,
                              int64_t timestamp, int flags)
{
    MXVDemuxContext *mxv = s->priv_data ;
    MXVTrack *tracks = NULL;
    AVStream *st = s->streams[stream_index];
    int i, index;

    /* Parse the CUES now since we need the index data to seek. */
    if (mxv->cues_parsing_deferred > 0) {
        mxv->cues_parsing_deferred = 0;
        mxv_parse_cues(mxv);
    }

    if (!st->nb_index_entries)
        goto err;
    timestamp = FFMAX(timestamp, st->index_entries[0].timestamp);

    if ((index = av_index_search_timestamp(st, timestamp, flags)) < 0 || index == st->nb_index_entries - 1) {
        mxv_reset_status(mxv, 0, st->index_entries[st->nb_index_entries - 1].pos);
        while ((index = av_index_search_timestamp(st, timestamp, flags)) < 0 || index == st->nb_index_entries - 1) {
            mxv_clear_queue(mxv);
            if (mxv_parse_cluster(mxv) < 0)
                break;
        }
    }

    mxv_clear_queue(mxv);
    if (index < 0 || (mxv->cues_parsing_deferred < 0 && index == st->nb_index_entries - 1))
        goto err;

    tracks = mxv->tracks.elem;
    for (i = 0; i < mxv->tracks.nb_elem; i++) {
        tracks[i].audio.pkt_cnt        = 0;
        tracks[i].audio.sub_packet_cnt = 0;
        tracks[i].audio.buf_timecode   = AV_NOPTS_VALUE;
        tracks[i].end_timecode         = 0;
    }

    /* We seek to a level 1 element, so set the appropriate status. */
    mxv_reset_status(mxv, 0, st->index_entries[index].pos);
    if (flags & AVSEEK_FLAG_ANY) {
        st->skip_to_keyframe = 0;
        mxv->skip_to_timecode = timestamp;
    } else {
        st->skip_to_keyframe = 1;
        mxv->skip_to_timecode = st->index_entries[index].timestamp;
    }
    mxv->skip_to_keyframe = 1;
    mxv->done             = 0;
    ff_update_cur_dts(s, st, st->index_entries[index].timestamp);
    return 0;
err:
    // slightly hackish but allows proper fallback to
    // the generic seeking code.
    mxv_reset_status(mxv, 0, -1);
    mxv->resync_pos = -1;
    mxv_clear_queue(mxv);
    st->skip_to_keyframe =
    mxv->skip_to_keyframe = 0;
    mxv->done = 0;
    return -1;
}

static int mxv_read_close(AVFormatContext *s)
{
    MXVDemuxContext *mxv = s->priv_data ;
    MXVTrack *tracks = mxv->tracks.elem;
    int n;

    av_free(mxv->aes_key);
    mxv_clear_queue(mxv);

    for (n = 0; n < mxv->tracks.nb_elem; n++)
        if (tracks[n].type == MXV_TRACK_TYPE_AUDIO)
            av_freep(&tracks[n].audio.buf);
    ebml_free(mxv_segment, mxv);

    return 0;
}

typedef struct {
    int64_t start_time_ns;
    int64_t end_time_ns;
    int64_t start_offset;
    int64_t end_offset;
} CueDesc;

/* This function searches all the Cues and returns the CueDesc corresponding to
 * the timestamp ts. Returned CueDesc will be such that start_time_ns <= ts <
 * end_time_ns. All 4 fields will be set to -1 if ts >= file's duration.
 */
static CueDesc get_cue_desc(AVFormatContext *s, int64_t ts, int64_t cues_start) {
    MXVDemuxContext *mxv = s->priv_data ;
    CueDesc cue_desc;
    int i;
    int nb_index_entries = s->streams[0]->nb_index_entries;
    AVIndexEntry *index_entries = s->streams[0]->index_entries;
    if (ts >= mxv->duration * mxv->time_scale) return (CueDesc) {-1, -1, -1, -1};
    for (i = 1; i < nb_index_entries; i++) {
        if (index_entries[i - 1].timestamp * mxv->time_scale <= ts &&
            index_entries[i].timestamp * mxv->time_scale > ts) {
            break;
        }
    }
    --i;
    cue_desc.start_time_ns = index_entries[i].timestamp * mxv->time_scale;
    cue_desc.start_offset = index_entries[i].pos - mxv->segment_start;
    if (i != nb_index_entries - 1) {
        cue_desc.end_time_ns = index_entries[i + 1].timestamp * mxv->time_scale;
        cue_desc.end_offset = index_entries[i + 1].pos - mxv->segment_start;
    } else {
        cue_desc.end_time_ns = mxv->duration * mxv->time_scale;
        // FIXME: this needs special handling for files where Cues appear
        // before Clusters. the current logic assumes Cues appear after
        // Clusters.
        cue_desc.end_offset = cues_start - mxv->segment_start;
    }
    return cue_desc;
}

static int webm_clusters_start_with_keyframe(AVFormatContext *s)
{
    MXVDemuxContext *mxv = s->priv_data ;
    uint32_t id = mxv->current_id;
    int64_t cluster_pos, before_pos;
    int index, rv = 1;
    if (s->streams[0]->nb_index_entries <= 0) return 0;
    // seek to the first cluster using cues.
    index = av_index_search_timestamp(s->streams[0], 0, 0);
    if (index < 0)  return 0;
    cluster_pos = s->streams[0]->index_entries[index].pos;
    before_pos = avio_tell(s->pb);
    while (1) {
        uint64_t cluster_id, cluster_length;
        int read;
        AVPacket *pkt;
        avio_seek(s->pb, cluster_pos, SEEK_SET);
        // read cluster id and length
        read = ebml_read_num(mxv, mxv->ctx->pb, 4, &cluster_id, 1);
        if (read < 0 || cluster_id != 0xF43B675) // done with all clusters
            break;
        read = ebml_read_length(mxv, mxv->ctx->pb, &cluster_length);
        if (read < 0)
            break;

        mxv_reset_status(mxv, 0, cluster_pos);
        mxv_clear_queue(mxv);
        if (mxv_parse_cluster(mxv) < 0 ||
            !mxv->queue) {
            break;
        }
        pkt = &mxv->queue->pkt;
        // 4 + read is the length of the cluster id and the cluster length field.
        cluster_pos += 4 + read + cluster_length;
        if (!(pkt->flags & AV_PKT_FLAG_KEY)) {
            rv = 0;
            break;
        }
    }

    /* Restore the status after mxv_read_header: */
    mxv_reset_status(mxv, id, before_pos);

    return rv;
}

static int buffer_size_after_time_downloaded(int64_t time_ns, double search_sec, int64_t bps,
                                             double min_buffer, double* buffer,
                                             double* sec_to_download, AVFormatContext *s,
                                             int64_t cues_start)
{
    double nano_seconds_per_second = 1000000000.0;
    double time_sec = time_ns / nano_seconds_per_second;
    int rv = 0;
    int64_t time_to_search_ns = (int64_t)(search_sec * nano_seconds_per_second);
    int64_t end_time_ns = time_ns + time_to_search_ns;
    double sec_downloaded = 0.0;
    CueDesc desc_curr = get_cue_desc(s, time_ns, cues_start);
    if (desc_curr.start_time_ns == -1)
      return -1;
    *sec_to_download = 0.0;

    // Check for non cue start time.
    if (time_ns > desc_curr.start_time_ns) {
      int64_t cue_nano = desc_curr.end_time_ns - time_ns;
      double percent = (double)(cue_nano) / (desc_curr.end_time_ns - desc_curr.start_time_ns);
      double cueBytes = (desc_curr.end_offset - desc_curr.start_offset) * percent;
      double timeToDownload = (cueBytes * 8.0) / bps;

      sec_downloaded += (cue_nano / nano_seconds_per_second) - timeToDownload;
      *sec_to_download += timeToDownload;

      // Check if the search ends within the first cue.
      if (desc_curr.end_time_ns >= end_time_ns) {
          double desc_end_time_sec = desc_curr.end_time_ns / nano_seconds_per_second;
          double percent_to_sub = search_sec / (desc_end_time_sec - time_sec);
          sec_downloaded = percent_to_sub * sec_downloaded;
          *sec_to_download = percent_to_sub * *sec_to_download;
      }

      if ((sec_downloaded + *buffer) <= min_buffer) {
          return 1;
      }

      // Get the next Cue.
      desc_curr = get_cue_desc(s, desc_curr.end_time_ns, cues_start);
    }

    while (desc_curr.start_time_ns != -1) {
        int64_t desc_bytes = desc_curr.end_offset - desc_curr.start_offset;
        int64_t desc_ns = desc_curr.end_time_ns - desc_curr.start_time_ns;
        double desc_sec = desc_ns / nano_seconds_per_second;
        double bits = (desc_bytes * 8.0);
        double time_to_download = bits / bps;

        sec_downloaded += desc_sec - time_to_download;
        *sec_to_download += time_to_download;

        if (desc_curr.end_time_ns >= end_time_ns) {
            double desc_end_time_sec = desc_curr.end_time_ns / nano_seconds_per_second;
            double percent_to_sub = search_sec / (desc_end_time_sec - time_sec);
            sec_downloaded = percent_to_sub * sec_downloaded;
            *sec_to_download = percent_to_sub * *sec_to_download;

            if ((sec_downloaded + *buffer) <= min_buffer)
                rv = 1;
            break;
        }

        if ((sec_downloaded + *buffer) <= min_buffer) {
            rv = 1;
            break;
        }

        desc_curr = get_cue_desc(s, desc_curr.end_time_ns, cues_start);
    }
    *buffer = *buffer + sec_downloaded;
    return rv;
}

/* This function computes the bandwidth of the WebM file with the help of
 * buffer_size_after_time_downloaded() function. Both of these functions are
 * adapted from WebM Tools project and are adapted to work with FFmpeg's
 * MXV parsing mechanism.
 *
 * Returns the bandwidth of the file on success; -1 on error.
 * */
static int64_t webm_dash_manifest_compute_bandwidth(AVFormatContext *s, int64_t cues_start)
{
    MXVDemuxContext *mxv = s->priv_data ;
    AVStream *st = s->streams[0];
    double bandwidth = 0.0;
    int i;

    for (i = 0; i < st->nb_index_entries; i++) {
        int64_t prebuffer_ns = 1000000000;
        int64_t time_ns = st->index_entries[i].timestamp * mxv->time_scale;
        double nano_seconds_per_second = 1000000000.0;
        int64_t prebuffered_ns = time_ns + prebuffer_ns;
        double prebuffer_bytes = 0.0;
        int64_t temp_prebuffer_ns = prebuffer_ns;
        int64_t pre_bytes, pre_ns;
        double pre_sec, prebuffer, bits_per_second;
        CueDesc desc_beg = get_cue_desc(s, time_ns, cues_start);

        // Start with the first Cue.
        CueDesc desc_end = desc_beg;

        // Figure out how much data we have downloaded for the prebuffer. This will
        // be used later to adjust the bits per sample to try.
        while (desc_end.start_time_ns != -1 && desc_end.end_time_ns < prebuffered_ns) {
            // Prebuffered the entire Cue.
            prebuffer_bytes += desc_end.end_offset - desc_end.start_offset;
            temp_prebuffer_ns -= desc_end.end_time_ns - desc_end.start_time_ns;
            desc_end = get_cue_desc(s, desc_end.end_time_ns, cues_start);
        }
        if (desc_end.start_time_ns == -1) {
            // The prebuffer is larger than the duration.
            if (mxv->duration * mxv->time_scale >= prebuffered_ns)
              return -1;
            bits_per_second = 0.0;
        } else {
            // The prebuffer ends in the last Cue. Estimate how much data was
            // prebuffered.
            pre_bytes = desc_end.end_offset - desc_end.start_offset;
            pre_ns = desc_end.end_time_ns - desc_end.start_time_ns;
            pre_sec = pre_ns / nano_seconds_per_second;
            prebuffer_bytes +=
                pre_bytes * ((temp_prebuffer_ns / nano_seconds_per_second) / pre_sec);

            prebuffer = prebuffer_ns / nano_seconds_per_second;

            // Set this to 0.0 in case our prebuffer buffers the entire video.
            bits_per_second = 0.0;
            do {
                int64_t desc_bytes = desc_end.end_offset - desc_beg.start_offset;
                int64_t desc_ns = desc_end.end_time_ns - desc_beg.start_time_ns;
                double desc_sec = desc_ns / nano_seconds_per_second;
                double calc_bits_per_second = (desc_bytes * 8) / desc_sec;

                // Drop the bps by the percentage of bytes buffered.
                double percent = (desc_bytes - prebuffer_bytes) / desc_bytes;
                double mod_bits_per_second = calc_bits_per_second * percent;

                if (prebuffer < desc_sec) {
                    double search_sec =
                        (double)(mxv->duration * mxv->time_scale) / nano_seconds_per_second;

                    // Add 1 so the bits per second should be a little bit greater than file
                    // datarate.
                    int64_t bps = (int64_t)(mod_bits_per_second) + 1;
                    const double min_buffer = 0.0;
                    double buffer = prebuffer;
                    double sec_to_download = 0.0;

                    int rv = buffer_size_after_time_downloaded(prebuffered_ns, search_sec, bps,
                                                               min_buffer, &buffer, &sec_to_download,
                                                               s, cues_start);
                    if (rv < 0) {
                        return -1;
                    } else if (rv == 0) {
                        bits_per_second = (double)(bps);
                        break;
                    }
                }

                desc_end = get_cue_desc(s, desc_end.end_time_ns, cues_start);
            } while (desc_end.start_time_ns != -1);
        }
        if (bandwidth < bits_per_second) bandwidth = bits_per_second;
    }
    return (int64_t)bandwidth;
}

static int webm_dash_manifest_cues(AVFormatContext *s, int64_t init_range)
{
    MXVDemuxContext *mxv = s->priv_data ;
    EbmlList *seekhead_list = &mxv->seekhead;
    MXVSeekhead *seekhead = seekhead_list->elem;
    char *buf;
    int64_t cues_start = -1, cues_end = -1, before_pos, bandwidth;
    int i;
    int end = 0;

    // determine cues start and end positions
    for (i = 0; i < seekhead_list->nb_elem; i++)
        if (seekhead[i].id == MXV_ID_CUES)
            break;

    if (i >= seekhead_list->nb_elem) return -1;

    before_pos = avio_tell(mxv->ctx->pb);
    cues_start = seekhead[i].pos + mxv->segment_start;
    if (avio_seek(mxv->ctx->pb, cues_start, SEEK_SET) == cues_start) {
        // cues_end is computed as cues_start + cues_length + length of the
        // Cues element ID (i.e. 4) + EBML length of the Cues element.
        // cues_end is inclusive and the above sum is reduced by 1.
        uint64_t cues_length, cues_id;
        int bytes_read;
        bytes_read = ebml_read_num   (mxv, mxv->ctx->pb, 4, &cues_id, 1);
        if (bytes_read < 0 || cues_id != (MXV_ID_CUES & 0xfffffff))
            return bytes_read < 0 ? bytes_read : AVERROR_INVALIDDATA;
        bytes_read = ebml_read_length(mxv, mxv->ctx->pb, &cues_length);
        if (bytes_read < 0)
            return bytes_read;
        cues_end = cues_start + 4 + bytes_read + cues_length - 1;
    }
    avio_seek(mxv->ctx->pb, before_pos, SEEK_SET);
    if (cues_start == -1 || cues_end == -1) return -1;

    // parse the cues
    mxv_parse_cues(mxv);

    // cues start
    av_dict_set_int(&s->streams[0]->metadata, CUES_START, cues_start, 0);

    // cues end
    av_dict_set_int(&s->streams[0]->metadata, CUES_END, cues_end, 0);

    // if the file has cues at the start, fix up the init range so that
    // it does not include it
    if (cues_start <= init_range)
        av_dict_set_int(&s->streams[0]->metadata, INITIALIZATION_RANGE, cues_start - 1, 0);

    // bandwidth
    bandwidth = webm_dash_manifest_compute_bandwidth(s, cues_start);
    if (bandwidth < 0) return -1;
    av_dict_set_int(&s->streams[0]->metadata, BANDWIDTH, bandwidth, 0);

    // check if all clusters start with key frames
    av_dict_set_int(&s->streams[0]->metadata, CLUSTER_KEYFRAME, webm_clusters_start_with_keyframe(s), 0);

    // store cue point timestamps as a comma separated list for checking subsegment alignment in
    // the muxer. assumes that each timestamp cannot be more than 20 characters long.
    buf = av_malloc_array(s->streams[0]->nb_index_entries, 20);
    if (!buf) return -1;
    strcpy(buf, "");
    for (i = 0; i < s->streams[0]->nb_index_entries; i++) {
        int ret = snprintf(buf + end, 20,
                           "%" PRId64"%s", s->streams[0]->index_entries[i].timestamp,
                           i != s->streams[0]->nb_index_entries - 1 ? "," : "");
        if (ret <= 0 || (ret == 20 && i ==  s->streams[0]->nb_index_entries - 1)) {
            av_log(s, AV_LOG_ERROR, "timestamp too long.\n");
            av_free(buf);
            return AVERROR_INVALIDDATA;
        }
        end += ret;
    }
    av_dict_set(&s->streams[0]->metadata, CUE_TIMESTAMPS, buf, 0);
    av_free(buf);

    return 0;
}

static int webm_dash_manifest_read_header(AVFormatContext *s)
{
    char *buf;
    int ret = mxv_read_header(s);
    int64_t init_range;
    MXVTrack *tracks;
    MXVDemuxContext *mxv = s->priv_data ;
    if (ret) {
        av_log(s, AV_LOG_ERROR, "Failed to read file headers\n");
        return -1;
    }
    if (!s->nb_streams) {
        mxv_read_close(s);
        av_log(s, AV_LOG_ERROR, "No streams found\n");
        return AVERROR_INVALIDDATA;
    }

    if (!mxv->is_live) {
        buf = av_asprintf("%g", mxv->duration);
        if (!buf) return AVERROR(ENOMEM);
        av_dict_set(&s->streams[0]->metadata, DURATION, buf, 0);
        av_free(buf);

        // initialization range
        // 5 is the offset of Cluster ID.
        init_range = avio_tell(s->pb) - 5;
        av_dict_set_int(&s->streams[0]->metadata, INITIALIZATION_RANGE, init_range, 0);
    }

    // basename of the file
    buf = strrchr(s->url, '/');
    av_dict_set(&s->streams[0]->metadata, FILENAME, buf ? ++buf : s->url, 0);

    // track number
    tracks = mxv->tracks.elem;
    av_dict_set_int(&s->streams[0]->metadata, TRACK_NUMBER, tracks[0].num, 0);

    // parse the cues and populate Cue related fields
    if (!mxv->is_live) {
        ret = webm_dash_manifest_cues(s, init_range);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Error parsing Cues\n");
            return ret;
        }
    }

    // use the bandwidth from the command line if it was provided
    if (mxv->bandwidth > 0) {
        av_dict_set_int(&s->streams[0]->metadata, BANDWIDTH,
                        mxv->bandwidth, 0);
    }
    return 0;
}

static int webm_dash_manifest_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    return AVERROR_EOF;
}

#define OFFSET(x) offsetof(MXVDemuxContext, x)
static const AVOption options[] = {
    { "live", "flag indicating that the input is a live file that only has the headers.", OFFSET(is_live), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    { "bandwidth", "bandwidth of this stream to be specified in the DASH manifest.", OFFSET(bandwidth), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};

AVInputFormat ff_mxv_demuxer = {
    .name           = "mxv",
    .long_name      = NULL_IF_CONFIG_SMALL("MXV Container"),
    .extensions     = "mxv",
    .priv_data_size = sizeof(MXVDemuxContext),
    .read_probe     = mxv_probe,
    .read_header    = mxv_read_header,
    .read_packet    = mxv_read_packet,
    .read_close     = mxv_read_close,
    .read_seek      = mxv_read_seek,
    .mime_type      = "audio/x-mxv,video/x-mxv"
};
#else

#include "mxv_wrap.h"

static int wraper_mxv_probe(const AVProbeData *p)
{
    return mxv_probe(p);
}
static int wraper_mxv_read_header(AVFormatContext *s)
{
    return mxv_read_header(s);
}

static int wraper_mxv_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    return mxv_read_packet(s, pkt);
}

static int wraper_mxv_read_close(AVFormatContext *s)
{
    return mxv_read_close(s);
}

static int wraper_mxv_read_seek(AVFormatContext *s, int stream_index,
                              int64_t timestamp, int flags)
{
    return mxv_read_seek(s, stream_index, timestamp, flags);
}

AVInputFormat ff_mxv_demuxer = {
    .name           = "mxv",
    .long_name      = NULL_IF_CONFIG_SMALL("MXV Container"),
    .extensions     = "mxv",
    .priv_data_size = 10240,
    .read_probe     = wraper_mxv_probe,
    .read_header    = wraper_mxv_read_header,
    .read_packet    = wraper_mxv_read_packet,
    .read_close     = wraper_mxv_read_close,
    .read_seek      = wraper_mxv_read_seek,
    .mime_type      = "audio/x-mxv,video/x-mxv"
};
#endif
