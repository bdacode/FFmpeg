/*
 * G.729 decoder
 * Copyright (c) 2008 Vladimir Voroshilov
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
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "avcodec.h"
#include "libavutil/avutil.h"
#include "get_bits.h"
#include "dsputil.h"

#include "g729.h"
#include "lsp.h"
#include "celp_math.h"
#include "acelp_filters.h"
#include "acelp_pitch_delay.h"
#include "acelp_vectors.h"
#include "g729data.h"

/**
 * minimum quantized LSF value (3.2.4)
 * 0.005 in Q13
 */
#define LSFQ_MIN                   40

/**
 * maximum quantized LSF value (3.2.4)
 * 3.135 in Q13
 */
#define LSFQ_MAX                   25681

/**
 * minimum LSF distance (3.2.4)
 * 0.0391 in Q13
 */
#define LSFQ_DIFF_MIN              321

/**
 * minimum gain pitch value (3.8, Equation 47)
 * 0.2 in (1.14)
 */
#define SHARP_MIN                  3277

/**
 * maximum gain pitch value (3.8, Equation 47)
 * (EE) This does not comply with the specification.
 * Specification says about 0.8, which should be
 * 13107 in (1.14), but reference C code uses
 * 13017 (equals to 0.7945) instead of it.
 */
#define SHARP_MAX                  13017

/**
 * MR_ENERGY (mean removed energy) = mean_energy + 10 * log10(2^26  * subframe_size) in (7.13)
 */
#define MR_ENERGY 1018156

typedef enum {
    FORMAT_G729_8K = 0,
    FORMAT_G729D_6K4,
    FORMAT_COUNT,
} G729Formats;

typedef struct {
    uint8_t ac_index_bits[2];   ///< adaptive codebook index for second subframe (size in bits)
    uint8_t parity_bit;         ///< parity bit for pitch delay
    uint8_t gc_1st_index_bits;  ///< gain codebook (first stage) index (size in bits)
    uint8_t gc_2nd_index_bits;  ///< gain codebook (second stage) index (size in bits)
    uint8_t fc_signs_bits;      ///< number of pulses in fixed-codebook vector
    uint8_t fc_indexes_bits;    ///< size (in bits) of fixed-codebook index entry
} G729FormatDescription;

typedef struct {
    DSPContext dsp;

    /// past excitation signal buffer
    int16_t exc_base[2*SUBFRAME_SIZE+PITCH_DELAY_MAX+INTERPOL_LEN];

    int16_t* exc;               ///< start of past excitation data in buffer
    int pitch_delay_int_prev;   ///< integer part of previous subframe's pitch delay (4.1.3)

    /// (2.13) LSP quantizer outputs
    int16_t  past_quantizer_output_buf[MA_NP + 1][10];
    int16_t* past_quantizer_outputs[MA_NP + 1];

    int16_t lsfq[10];           ///< (2.13) quantized LSF coefficients from previous frame
    int16_t lsp_buf[2][10];     ///< (0.15) LSP coefficients (previous and current frames) (3.2.5)
    int16_t *lsp[2];            ///< pointers to lsp_buf

    int16_t quant_energy[4];    ///< (5.10) past quantized energy

    /// (1.14) pitch gain of previous subframe
    int16_t gain_pitch;

    /// (14.1) gain code from previous subframe
    int16_t gain_code;

    int16_t was_periodic;       ///< whether previous frame was declared as periodic or not (4.4)
    uint16_t rand_value;        ///< random number generator value (4.4.4)
    int ma_predictor_prev;      ///< switched MA predictor of LSP quantizer from last good frame
}  G729Context;

static const G729FormatDescription format_g729_8k = {
    .ac_index_bits     = {8,5},
    .parity_bit        = 1,
    .gc_1st_index_bits = GC_1ST_IDX_BITS_8K,
    .gc_2nd_index_bits = GC_2ND_IDX_BITS_8K,
    .fc_signs_bits     = 4,
    .fc_indexes_bits   = 13,
};

static const G729FormatDescription format_g729d_6k4 = {
    .ac_index_bits     = {8,4},
    .parity_bit        = 0,
    .gc_1st_index_bits = GC_1ST_IDX_BITS_6K4,
    .gc_2nd_index_bits = GC_2ND_IDX_BITS_6K4,
    .fc_signs_bits     = 2,
    .fc_indexes_bits   = 9,
};

/**
 * @brief pseudo random number generator
 */
static inline uint16_t g729_prng(uint16_t value)
{
    return 31821 * value + 13849;
}

/**
 * Get parity bit of bit 2..7
 */
static inline int get_parity(uint8_t value)
{
   return (0x6996966996696996ULL >> (value >> 2)) & 1;
}

/*
 * Decodes LSF (Line Spectral Frequencies) from L0-L3 (3.2.4).
 * @param lsfq [out] (2.13) quantized LSF coefficients
 * @param past_quantizer_outputs [in/out] (2.13) quantizer outputs from previous frames
 * @param ma_predictor switched MA predictor of LSP quantizer
 * @param vq_1st first stage vector of quantizer
 * @param vq_2nd_low second stage lower vector of LSP quantizer
 * @param vq_2nd_high second stage higher vector of LSP quantizer
 */
static void lsf_decode(int16_t* lsfq, int16_t* past_quantizer_outputs[MA_NP + 1],
                       int16_t ma_predictor,
                       int16_t vq_1st, int16_t vq_2nd_low, int16_t vq_2nd_high)
{
    int i,j;
    static const uint8_t min_distance[2]={10, 5}; //(2.13)
    int16_t* quantizer_output = past_quantizer_outputs[MA_NP];

    for (i = 0; i < 5; i++) {
        quantizer_output[i]     = cb_lsp_1st[vq_1st][i    ] + cb_lsp_2nd[vq_2nd_low ][i    ];
        quantizer_output[i + 5] = cb_lsp_1st[vq_1st][i + 5] + cb_lsp_2nd[vq_2nd_high][i + 5];
    }

    for (j = 0; j < 2; j++) {
        for (i = 1; i < 10; i++) {
            int diff = (quantizer_output[i - 1] - quantizer_output[i] + min_distance[j]) >> 1;
            if (diff > 0) {
                quantizer_output[i - 1] -= diff;
                quantizer_output[i    ] += diff;
            }
        }
    }

    for (i = 0; i < 10; i++) {
        int sum = quantizer_output[i] * cb_ma_predictor_sum[ma_predictor][i];
        for (j = 0; j < MA_NP; j++)
            sum += past_quantizer_outputs[j][i] * cb_ma_predictor[ma_predictor][j][i];

        lsfq[i] = sum >> 15;
    }

    ff_acelp_reorder_lsf(lsfq, LSFQ_DIFF_MIN, LSFQ_MIN, LSFQ_MAX, 10);
}

/**
 * Restores past LSP quantizer output using LSF from previous frame
 * @param lsfq [in/out] (2.13) quantized LSF coefficients
 * @param past_quantizer_outputs [in/out] (2.13) quantizer outputs from previous frames
 * @param ma_predictor_prev MA predictor from previous frame
 * @param lsfq_prev (2.13) quantized LSF coefficients from previous frame
 */
static void lsf_restore_from_previous(int16_t* lsfq,
                                      int16_t* past_quantizer_outputs[MA_NP + 1],
                                      int ma_predictor_prev)
{
    int16_t* quantizer_output = past_quantizer_outputs[MA_NP];
    int i,k;

    for (i = 0; i < 10; i++) {
        int tmp = lsfq[i] << 15;

        for (k = 0; k < MA_NP; k++)
            tmp -= past_quantizer_outputs[k][i] * cb_ma_predictor[ma_predictor_prev][k][i];

        quantizer_output[i] = ((tmp >> 15) * cb_ma_predictor_sum_inv[ma_predictor_prev][i]) >> 12;
    }
}

static av_cold int decoder_init(AVCodecContext * avctx)
{
    G729Context* ctx = avctx->priv_data;
    int i,k;

    if (avctx->channels != 1) {
        av_log(avctx, AV_LOG_ERROR, "Only mono sound is supported (requested channels: %d).\n", avctx->channels);
        return AVERROR(EINVAL);
    }

    /* Both 8kbit/s and 6.4kbit/s modes uses two subframes per frame. */
    avctx->frame_size = SUBFRAME_SIZE << 1;

    for (k = 0; k < MA_NP + 1; k++) {
        ctx->past_quantizer_outputs[k] = ctx->past_quantizer_output_buf[k];
        for (i = 1; i < 11; i++)
            ctx->past_quantizer_outputs[k][i - 1] = (18717 * i) >> 3;
    }

    ctx->lsp[0] = ctx->lsp_buf[0];
    ctx->lsp[1] = ctx->lsp_buf[1];
    memcpy(ctx->lsp[0], lsp_init, 10 * sizeof(int16_t));

    ctx->exc = &ctx->exc_base[PITCH_DELAY_MAX+INTERPOL_LEN];

    /* random seed initialization */
    ctx->rand_value = 21845;

    /* quantized prediction error */
    for(i=0; i<4; i++)
        ctx->quant_energy[i] = -14336; // -14 in (5.10)

    dsputil_init(&ctx->dsp, avctx);

    return 0;
}

static int decode_frame(AVCodecContext *avctx, void *data, int *data_size,
                        AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int buf_size       = avpkt->size;
    int16_t *out_frame = data;
    GetBitContext gb;
    G729FormatDescription format;
    int frame_erasure = 0;    ///< frame erasure detected during decoding
    int bad_pitch = 0;        ///< parity check failed
    int i;
    int16_t *tmp;
    G729Formats packet_type;
    G729Context *ctx = avctx->priv_data;
    int16_t lp[2][11];           // (3.12)
    uint8_t ma_predictor;     ///< switched MA predictor of LSP quantizer
    uint8_t quantizer_1st;    ///< first stage vector of quantizer
    uint8_t quantizer_2nd_lo; ///< second stage lower vector of quantizer (size in bits)
    uint8_t quantizer_2nd_hi; ///< second stage higher vector of quantizer (size in bits)

    int pitch_delay_int;         // pitch delay, integer part
    int pitch_delay_3x;          // pitch delay, multiplied by 3
    int16_t fc[SUBFRAME_SIZE];   // fixed-codebook vector
    int is_periodic = 0;         // whether one of the subframes is declared as periodic or not

    if (*data_size < SUBFRAME_SIZE << 2) {
        av_log(avctx, AV_LOG_ERROR, "Error processing packet: output buffer too small\n");
        return AVERROR(EIO);
    }

    if (buf_size == 10) {
        packet_type = FORMAT_G729_8K;
        format = format_g729_8k;
        av_log(avctx, AV_LOG_DEBUG, "Packet type: %s\n", "G.729 @ 8kbit/s");
    } else if (buf_size == 8) {
        packet_type = FORMAT_G729D_6K4;
        format = format_g729d_6k4;
        av_log(avctx, AV_LOG_DEBUG, "Packet type: %s\n", "G.729D @ 6.4kbit/s");
    } else {
        av_log(avctx, AV_LOG_ERROR, "Packet size %d is unknown.\n", buf_size);
        return AVERROR_INVALIDDATA;
    }

    for (i=0; i < buf_size; i++)
        frame_erasure |= buf[i];
    frame_erasure = !frame_erasure;

    init_get_bits(&gb, buf, buf_size);

    ma_predictor     = get_bits(&gb, 1);
    quantizer_1st    = get_bits(&gb, VQ_1ST_BITS);
    quantizer_2nd_lo = get_bits(&gb, VQ_2ND_BITS);
    quantizer_2nd_hi = get_bits(&gb, VQ_2ND_BITS);

    if(frame_erasure)
        lsf_restore_from_previous(ctx->lsfq, ctx->past_quantizer_outputs,
                                  ctx->ma_predictor_prev);
    else {
        lsf_decode(ctx->lsfq, ctx->past_quantizer_outputs,
                   ma_predictor,
                   quantizer_1st, quantizer_2nd_lo, quantizer_2nd_hi);
        ctx->ma_predictor_prev = ma_predictor;
    }

    tmp = ctx->past_quantizer_outputs[MA_NP];
    memmove(ctx->past_quantizer_outputs + 1, ctx->past_quantizer_outputs,
            MA_NP * sizeof(int16_t*));
    ctx->past_quantizer_outputs[0] = tmp;

    ff_acelp_lsf2lsp(ctx->lsp[1], ctx->lsfq, 10);

    ff_acelp_lp_decode(&lp[0][0], &lp[1][0], ctx->lsp[1], ctx->lsp[0], 10);

    FFSWAP(int16_t*, ctx->lsp[1], ctx->lsp[0]);

    for (i = 0; i < 2; i++) {
        int gain_corr_factor;

        uint8_t ac_index;      ///< adaptive codebook index
        uint8_t pulses_signs;  ///< fixed-codebook vector pulse signs
        int fc_indexes;        ///< fixed-codebook indexes
        uint8_t gc_1st_index;  ///< gain codebook (first stage) index
        uint8_t gc_2nd_index;  ///< gain codebook (second stage) index

        ac_index      = get_bits(&gb, format.ac_index_bits[i]);
        if(!i && format.parity_bit)
            bad_pitch = get_parity(ac_index) == get_bits1(&gb);
        fc_indexes    = get_bits(&gb, format.fc_indexes_bits);
        pulses_signs  = get_bits(&gb, format.fc_signs_bits);
        gc_1st_index  = get_bits(&gb, format.gc_1st_index_bits);
        gc_2nd_index  = get_bits(&gb, format.gc_2nd_index_bits);

        if (frame_erasure)
            pitch_delay_3x   = 3 * ctx->pitch_delay_int_prev;
        else if(!i) {
            if (bad_pitch)
                pitch_delay_3x   = 3 * ctx->pitch_delay_int_prev;
            else
                pitch_delay_3x = ff_acelp_decode_8bit_to_1st_delay3(ac_index);
        } else {
            int pitch_delay_min = av_clip(ctx->pitch_delay_int_prev - 5,
                                          PITCH_DELAY_MIN, PITCH_DELAY_MAX - 9);

            if(packet_type == FORMAT_G729D_6K4)
                pitch_delay_3x = ff_acelp_decode_4bit_to_2nd_delay3(ac_index, pitch_delay_min);
            else
                pitch_delay_3x = ff_acelp_decode_5_6_bit_to_2nd_delay3(ac_index, pitch_delay_min);
        }

        /* Round pitch delay to nearest (used everywhere except ff_acelp_interpolate). */
        pitch_delay_int  = (pitch_delay_3x + 1) / 3;

        if (frame_erasure) {
            ctx->rand_value = g729_prng(ctx->rand_value);
            fc_indexes   = ctx->rand_value & ((1 << format.fc_indexes_bits) - 1);

            ctx->rand_value = g729_prng(ctx->rand_value);
            pulses_signs = ctx->rand_value;
        }


        memset(fc, 0, sizeof(int16_t) * SUBFRAME_SIZE);
        switch (packet_type) {
            case FORMAT_G729_8K:
                ff_acelp_fc_pulse_per_track(fc, ff_fc_4pulses_8bits_tracks_13,
                                            ff_fc_4pulses_8bits_track_4,
                                            fc_indexes, pulses_signs, 3, 3);
                break;
            case FORMAT_G729D_6K4:
                ff_acelp_fc_pulse_per_track(fc, ff_fc_2pulses_9bits_track1_gray,
                                            ff_fc_2pulses_9bits_track2_gray,
                                            fc_indexes, pulses_signs, 1, 4);
                break;
        }

        /*
          This filter enhances harmonic components of the fixed-codebook vector to
          improve the quality of the reconstructed speech.

                     / fc_v[i],                                    i < pitch_delay
          fc_v[i] = <
                     \ fc_v[i] + gain_pitch * fc_v[i-pitch_delay], i >= pitch_delay
        */
        ff_acelp_weighted_vector_sum(fc + pitch_delay_int,
                                     fc + pitch_delay_int,
                                     fc, 1 << 14,
                                     av_clip(ctx->gain_pitch, SHARP_MIN, SHARP_MAX),
                                     0, 14,
                                     SUBFRAME_SIZE - pitch_delay_int);

        if (frame_erasure) {
            ctx->gain_pitch = (29491 * ctx->gain_pitch) >> 15; // 0.90 (0.15)
            ctx->gain_code  = ( 2007 * ctx->gain_code ) >> 11; // 0.98 (0.11)

            gain_corr_factor = 0;
        } else {
            ctx->gain_pitch  = cb_gain_1st_8k[gc_1st_index][0] +
                               cb_gain_2nd_8k[gc_2nd_index][0];
            gain_corr_factor = cb_gain_1st_8k[gc_1st_index][1] +
                               cb_gain_2nd_8k[gc_2nd_index][1];

            /* Decode the fixed-codebook gain. */
            ctx->gain_code = ff_acelp_decode_gain_code(&ctx->dsp, gain_corr_factor,
                                                       fc, MR_ENERGY,
                                                       ctx->quant_energy,
                                                       ma_prediction_coeff,
                                                       SUBFRAME_SIZE, 4);
        }
        ff_acelp_update_past_gain(ctx->quant_energy, gain_corr_factor, 2, frame_erasure);

        /* Routine requires rounding to lowest. */
        ff_acelp_interpolate(ctx->exc + i * SUBFRAME_SIZE,
                             ctx->exc + i * SUBFRAME_SIZE - pitch_delay_3x / 3,
                             ff_acelp_interp_filter, 6,
                             (pitch_delay_3x % 3) << 1,
                             10, SUBFRAME_SIZE);

        ff_acelp_weighted_vector_sum(ctx->exc + i * SUBFRAME_SIZE,
                                     ctx->exc + i * SUBFRAME_SIZE, fc,
                                     (!ctx->was_periodic && frame_erasure) ? 0 : ctx->gain_pitch,
                                     ( ctx->was_periodic && frame_erasure) ? 0 : ctx->gain_code,
                                     1 << 13, 14, SUBFRAME_SIZE);

        if (frame_erasure)
            ctx->pitch_delay_int_prev = FFMIN(ctx->pitch_delay_int_prev + 1, PITCH_DELAY_MAX);
        else
            ctx->pitch_delay_int_prev = pitch_delay_int;
    }

    ctx->was_periodic = is_periodic;

    /* Save signal for use in next frame. */
    memmove(ctx->exc_base, ctx->exc_base + 2 * SUBFRAME_SIZE, (PITCH_DELAY_MAX+INTERPOL_LEN)*sizeof(int16_t));

    *data_size = SUBFRAME_SIZE << 2;
    return buf_size;
}

AVCodec ff_g729_decoder =
{
    "g729",
    AVMEDIA_TYPE_AUDIO,
    CODEC_ID_G729,
    sizeof(G729Context),
    decoder_init,
    NULL,
    NULL,
    decode_frame,
    .long_name = NULL_IF_CONFIG_SMALL("G.729"),
};
