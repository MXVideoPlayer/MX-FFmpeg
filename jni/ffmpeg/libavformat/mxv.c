//
//  mxv.c
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

#include "mxv.h"
#include "libavutil/stereo3d.h"
#include "libavutil/aes.h"

/* If you add a tag here that is not in ff_codec_bmp_tags[]
   or ff_codec_wav_tags[], add it also to additional_audio_tags[]
   or additional_video_tags[] in matroskaenc.c */
const CodecTags ff_mxv_codec_tags[]={
    {"A_AAC"            , AV_CODEC_ID_AAC},
    {"A_AC3"            , AV_CODEC_ID_AC3},
    {"A_ALAC"           , AV_CODEC_ID_ALAC},
    {"A_DTS"            , AV_CODEC_ID_DTS},
    {"A_EAC3"           , AV_CODEC_ID_EAC3},
    {"A_FLAC"           , AV_CODEC_ID_FLAC},
    {"A_MLP"            , AV_CODEC_ID_MLP},
    {"A_MPEG/L2"        , AV_CODEC_ID_MP2},
    {"A_MPEG/L1"        , AV_CODEC_ID_MP1},
    {"A_MPEG/L3"        , AV_CODEC_ID_MP3},
    {"A_OPUS"           , AV_CODEC_ID_OPUS},
    {"A_OPUS/EXPERIMENTAL",AV_CODEC_ID_OPUS},
    {"A_PCM/FLOAT/IEEE" , AV_CODEC_ID_PCM_F32LE},
    {"A_PCM/FLOAT/IEEE" , AV_CODEC_ID_PCM_F64LE},
    {"A_PCM/INT/BIG"    , AV_CODEC_ID_PCM_S16BE},
    {"A_PCM/INT/BIG"    , AV_CODEC_ID_PCM_S24BE},
    {"A_PCM/INT/BIG"    , AV_CODEC_ID_PCM_S32BE},
    {"A_PCM/INT/LIT"    , AV_CODEC_ID_PCM_S16LE},
    {"A_PCM/INT/LIT"    , AV_CODEC_ID_PCM_S24LE},
    {"A_PCM/INT/LIT"    , AV_CODEC_ID_PCM_S32LE},
    {"A_PCM/INT/LIT"    , AV_CODEC_ID_PCM_U8},
    {"A_QUICKTIME/QDMC" , AV_CODEC_ID_QDMC},
    {"A_QUICKTIME/QDM2" , AV_CODEC_ID_QDM2},
    {"A_REAL/14_4"      , AV_CODEC_ID_RA_144},
    {"A_REAL/28_8"      , AV_CODEC_ID_RA_288},
    {"A_REAL/ATRC"      , AV_CODEC_ID_ATRAC3},
    {"A_REAL/COOK"      , AV_CODEC_ID_COOK},
    {"A_REAL/SIPR"      , AV_CODEC_ID_SIPR},
    {"A_TRUEHD"         , AV_CODEC_ID_TRUEHD},
    {"A_TTA1"           , AV_CODEC_ID_TTA},
    {"A_VORBIS"         , AV_CODEC_ID_VORBIS},
    {"A_WAVPACK4"       , AV_CODEC_ID_WAVPACK},

    {"D_WEBVTT/SUBTITLES"   , AV_CODEC_ID_WEBVTT},
    {"D_WEBVTT/CAPTIONS"    , AV_CODEC_ID_WEBVTT},
    {"D_WEBVTT/DESCRIPTIONS", AV_CODEC_ID_WEBVTT},
    {"D_WEBVTT/METADATA"    , AV_CODEC_ID_WEBVTT},

    {"S_TEXT/UTF8"      , AV_CODEC_ID_SUBRIP},
    {"S_TEXT/UTF8"      , AV_CODEC_ID_TEXT},
    {"S_TEXT/ASCII"     , AV_CODEC_ID_TEXT},
    {"S_TEXT/ASS"       , AV_CODEC_ID_ASS},
    {"S_TEXT/SSA"       , AV_CODEC_ID_ASS},
    {"S_ASS"            , AV_CODEC_ID_ASS},
    {"S_SSA"            , AV_CODEC_ID_ASS},
    {"S_VOBSUB"         , AV_CODEC_ID_DVD_SUBTITLE},
    {"S_DVBSUB"         , AV_CODEC_ID_DVB_SUBTITLE},
    {"S_HDMV/PGS"       , AV_CODEC_ID_HDMV_PGS_SUBTITLE},
    {"S_HDMV/TEXTST"    , AV_CODEC_ID_HDMV_TEXT_SUBTITLE},

    {"V_AV1"            , AV_CODEC_ID_AV1},
    {"V_DIRAC"          , AV_CODEC_ID_DIRAC},
    {"V_FFV1"           , AV_CODEC_ID_FFV1},
    {"V_MJPEG"          , AV_CODEC_ID_MJPEG},
    {"V_MPEG1"          , AV_CODEC_ID_MPEG1VIDEO},
    {"V_MPEG2"          , AV_CODEC_ID_MPEG2VIDEO},
    {"V_MPEG4/ISO/ASP"  , AV_CODEC_ID_MPEG4},
    {"V_MPEG4/ISO/AP"   , AV_CODEC_ID_MPEG4},
    {"V_MPEG4/ISO/SP"   , AV_CODEC_ID_MPEG4},
    {"V_MPEG4/ISO/AVC"  , AV_CODEC_ID_H264},
    {"V_MPEGH/ISO/HEVC" , AV_CODEC_ID_HEVC},
    {"V_MPEG4/MS/V3"    , AV_CODEC_ID_MSMPEG4V3},
    {"V_PRORES"         , AV_CODEC_ID_PRORES},
    {"V_REAL/RV10"      , AV_CODEC_ID_RV10},
    {"V_REAL/RV20"      , AV_CODEC_ID_RV20},
    {"V_REAL/RV30"      , AV_CODEC_ID_RV30},
    {"V_REAL/RV40"      , AV_CODEC_ID_RV40},
    {"V_SNOW"           , AV_CODEC_ID_SNOW},
    {"V_THEORA"         , AV_CODEC_ID_THEORA},
    {"V_UNCOMPRESSED"   , AV_CODEC_ID_RAWVIDEO},
    {"V_VP8"            , AV_CODEC_ID_VP8},
    {"V_VP9"            , AV_CODEC_ID_VP9},

    {""                 , AV_CODEC_ID_NONE}
};

//const CodecTags ff_webm_codec_tags[] = {
//    {"V_VP8"            , AV_CODEC_ID_VP8},
//    {"V_VP9"            , AV_CODEC_ID_VP9},
//    {"V_AV1"            , AV_CODEC_ID_AV1},
//
//    {"A_VORBIS"         , AV_CODEC_ID_VORBIS},
//    {"A_OPUS"           , AV_CODEC_ID_OPUS},
//
//    {"D_WEBVTT/SUBTITLES"   , AV_CODEC_ID_WEBVTT},
//    {"D_WEBVTT/CAPTIONS"    , AV_CODEC_ID_WEBVTT},
//    {"D_WEBVTT/DESCRIPTIONS", AV_CODEC_ID_WEBVTT},
//    {"D_WEBVTT/METADATA"    , AV_CODEC_ID_WEBVTT},
//
//    {""                 , AV_CODEC_ID_NONE}
//};

const CodecMime ff_mxv_image_mime_tags[] = {
    {"image/gif"                  , AV_CODEC_ID_GIF},
    {"image/jpeg"                 , AV_CODEC_ID_MJPEG},
    {"image/png"                  , AV_CODEC_ID_PNG},
    {"image/tiff"                 , AV_CODEC_ID_TIFF},

    {""                           , AV_CODEC_ID_NONE}
};

const CodecMime ff_mxv_mime_tags[] = {
    {"text/plain"                 , AV_CODEC_ID_TEXT},
    {"application/x-truetype-font", AV_CODEC_ID_TTF},
    {"application/x-font"         , AV_CODEC_ID_TTF},
    {"application/vnd.ms-opentype", AV_CODEC_ID_OTF},
    {"binary"                     , AV_CODEC_ID_BIN_DATA},

    {""                           , AV_CODEC_ID_NONE}
};

const AVMetadataConv ff_mxv_metadata_conv[] = {
    { "LEAD_PERFORMER", "performer" },
    { "PART_NUMBER"   , "track"  },
    { 0 }
};

const char * const ff_mxv_video_stereo_mode[MXV_VIDEO_STEREOMODE_TYPE_NB] = {
    "mono",
    "left_right",
    "bottom_top",
    "top_bottom",
    "checkerboard_rl",
    "checkerboard_lr",
    "row_interleaved_rl",
    "row_interleaved_lr",
    "col_interleaved_rl",
    "col_interleaved_lr",
    "anaglyph_cyan_red",
    "right_left",
    "anaglyph_green_magenta",
    "block_lr",
    "block_rl",
};

const char * const ff_mxv_video_stereo_plane[MXV_VIDEO_STEREO_PLANE_COUNT] = {
    "left",
    "right",
    "background",
};

int ff_mxv_stereo3d_conv(AVStream *st, MXVVideoStereoModeType stereo_mode)
{
    AVStereo3D *stereo;
    int ret;

    stereo = av_stereo3d_alloc();
    if (!stereo)
        return AVERROR(ENOMEM);

    // note: the missing breaks are intentional
    switch (stereo_mode) {
    case MXV_VIDEO_STEREOMODE_TYPE_MONO:
        stereo->type = AV_STEREO3D_2D;
        break;
    case MXV_VIDEO_STEREOMODE_TYPE_RIGHT_LEFT:
        stereo->flags |= AV_STEREO3D_FLAG_INVERT;
    case MXV_VIDEO_STEREOMODE_TYPE_LEFT_RIGHT:
        stereo->type = AV_STEREO3D_SIDEBYSIDE;
        break;
    case MXV_VIDEO_STEREOMODE_TYPE_BOTTOM_TOP:
        stereo->flags |= AV_STEREO3D_FLAG_INVERT;
    case MXV_VIDEO_STEREOMODE_TYPE_TOP_BOTTOM:
        stereo->type = AV_STEREO3D_TOPBOTTOM;
        break;
    case MXV_VIDEO_STEREOMODE_TYPE_CHECKERBOARD_RL:
        stereo->flags |= AV_STEREO3D_FLAG_INVERT;
    case MXV_VIDEO_STEREOMODE_TYPE_CHECKERBOARD_LR:
        stereo->type = AV_STEREO3D_CHECKERBOARD;
        break;
    case MXV_VIDEO_STEREOMODE_TYPE_ROW_INTERLEAVED_RL:
        stereo->flags |= AV_STEREO3D_FLAG_INVERT;
    case MXV_VIDEO_STEREOMODE_TYPE_ROW_INTERLEAVED_LR:
        stereo->type = AV_STEREO3D_LINES;
        break;
    case MXV_VIDEO_STEREOMODE_TYPE_COL_INTERLEAVED_RL:
        stereo->flags |= AV_STEREO3D_FLAG_INVERT;
    case MXV_VIDEO_STEREOMODE_TYPE_COL_INTERLEAVED_LR:
        stereo->type = AV_STEREO3D_COLUMNS;
        break;
    case MXV_VIDEO_STEREOMODE_TYPE_BOTH_EYES_BLOCK_RL:
        stereo->flags |= AV_STEREO3D_FLAG_INVERT;
    case MXV_VIDEO_STEREOMODE_TYPE_BOTH_EYES_BLOCK_LR:
        stereo->type = AV_STEREO3D_FRAMESEQUENCE;
        break;
    }

    ret = av_stream_add_side_data(st, AV_PKT_DATA_STEREO3D, (uint8_t *)stereo,
                                  sizeof(*stereo));
    if (ret < 0) {
        av_freep(&stereo);
        return ret;
    }

    return 0;
}

static const uint8_t *MXPlayer_HardCodeKey = "MXPayer is the best player ever.";
static int oneBlockSize = 16;
void printBuffer( const uint8_t* buffer, int size )
{
//    av_log( NULL, AV_LOG_ERROR, "%s\n", buffer );
    for ( int i = 0; i < size; ++i )
    {
        av_log( NULL, AV_LOG_ERROR, "0x%x ", buffer[i] );
    }
    av_log( NULL, AV_LOG_ERROR, "\n" );
}

void ff_mxv_generate_aes_key(uint8_t *key, int key_size)
{
    int i,flag;
    srand( time( NULL ) );
    for(i = 0; i < key_size - 1; i ++)
    {
        flag = rand()%3;
        switch(flag)
        {
        case 0:
            key[i] = rand()%26 + 'a';
            break;
        case 1:
            key[i] = rand()%26 + 'A';
            break;
        case 2:
            key[i] = rand()%10 + '0';
            break;
        }
    }
    key[++i] = '\0';
    printf("---------------%s--------------\n", key);//输出生成的随机数。
}

void ff_mxv_encrypt_aes128(uint8_t *output, const uint8_t *key, const uint8_t *input, int size)
{

    uint8_t *encInput = NULL;
    uint8_t *inputPadding = NULL;
    uint8_t *encOutput = NULL;

    int paddingSize = size % oneBlockSize;
    int encrytSize = size - paddingSize;

    struct AVAES *encrypt = av_aes_alloc();
    uint8_t *encKey = av_malloc(TRACK_ENCRYPTION_KEY_SIZE);
    memcpy(encKey, key, TRACK_ENCRYPTION_KEY_SIZE);
    av_aes_init(encrypt, encKey, 128, 0);

    if (paddingSize == 0) {
        av_aes_crypt(encrypt, output, input, size >> 4, NULL, 0);
    }
    else {
        encInput = av_mallocz(encrytSize);
        inputPadding = av_mallocz(paddingSize);

        memcpy(encInput, input, encrytSize);
        memcpy(inputPadding, input+encrytSize, paddingSize);

        av_aes_crypt(encrypt, output, encInput, encrytSize >> 4, NULL, 0);

        memcpy(output+encrytSize, inputPadding, paddingSize);

        av_free(encInput);
        av_free(inputPadding);
    }
    av_free(encKey);
}

void ff_mxv_decrypt_aes128(uint8_t *output, const uint8_t *key, const uint8_t *input, int size)
{
    int paddingSize = size & ( oneBlockSize - 1 );
    int decryptSize = size - paddingSize;

    struct AVAES *decrypt = av_aes_alloc();
    if (decrypt) {
        av_aes_init(decrypt, key, 128, 1);
        av_aes_crypt(decrypt, output, input, decryptSize >> 4, NULL, 1);
        if (paddingSize > 0 && input != output) {
            memcpy(output + decryptSize, input + decryptSize, paddingSize);
        }
        av_free(decrypt);
    }
}

//unsigned char key[16] = {0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5};
//unsigned char iv[16]  = {0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5};
//
//void ff_mxv_encrypt_ssl_aes128(uint8_t *output, const uint8_t *input, int size)
//{
//    AES_KEY enc_key;
//
//    AES_set_encrypt_key(key, 128, &enc_key);
//
//    AES_ecb_encrypt(input, output, &enc_key, AES_ENCRYPT);
//
////    AES_cbc_encrypt(input, output, size, &enc_key, iv, AES_ENCRYPT);
//
//
//    if (size == 66) {
//        printf("------clear-----\n");
//        printf(input);
//        printf("------unclear------\n");
//        printf(output);
//    }
//}
//
//void ff_mxv_decrypt_ssl_aes128(uint8_t *output, const uint8_t *input, int size)
//{
//    AES_KEY dec_key;
//
//    AES_set_decrypt_key(key, 128, &dec_key);
//
//    AES_cbc_encrypt(input, output, size, &dec_key, iv, AES_DECRYPT);
//
//    if (size == 66) {
//        printf("------unclear------");
//        printBuffer(input, size);
//        printf("------clear--------");
//        printBuffer(output, size);
//    }
//}
