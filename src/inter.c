/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Copyright (C) 2013-2015 Tampere University of Technology and others (see
 * COPYING file).
 *
 * Kvazaar is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * Kvazaar is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Kvazaar.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/

#include "inter.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "encoder.h"
#include "imagelist.h"
#include "strategies/generic/picture-generic.h"
#include "strategies/strategies-ipol.h"
#include "videoframe.h"
#include "strategies/strategies-picture.h"


const int8_t kvz_g_imv_to_prec[4] = { 2, 0, -2, 1 }; // Convert AMVR imv to precision

typedef struct {
  const cu_info_t *a[2];
  const cu_info_t *b[3];
  const cu_info_t *c3;
  const cu_info_t *h;

  uint16_t mer_a0[2];
  uint16_t mer_a1[2];

  uint16_t mer_b0[2];
  uint16_t mer_b1[2];
  uint16_t mer_b2[2];

  uint16_t mer_c3[2];
  uint16_t mer_h[2];

} merge_candidates_t;


static void inter_recon_frac_luma(const encoder_state_t *const state,
  const kvz_picture *const ref,
  int32_t xpos,
  int32_t ypos,
  int32_t block_width,
  int32_t block_height,
  const int16_t mv_param[2],
  lcu_t *lcu)
{
  int mv_frac_x = (mv_param[0] & 3);
  int mv_frac_y = (mv_param[1] & 3);

  // Space for extrapolated pixels and the part from the picture.
  // Some extra for AVX2.
  // The extrapolation function will set the pointers and stride.
  kvz_pixel ext_buffer[KVZ_IPOL_MAX_INPUT_SIZE_LUMA_SIMD];
  kvz_pixel *ext = NULL;
  kvz_pixel *ext_origin = NULL;
  int ext_s = 0;
  kvz_epol_args epol_args = {
    .src = ref->y,
    .src_w = ref->width,
    .src_h = ref->height,
    .src_s = ref->stride,
    .blk_x = state->tile->offset_x + xpos + (mv_param[0] >> 2),
    .blk_y = state->tile->offset_y + ypos + (mv_param[1] >> 2),
    .blk_w = block_width,
    .blk_h = block_height,
    .pad_l = KVZ_LUMA_FILTER_OFFSET,
    .pad_r = KVZ_EXT_PADDING_LUMA - KVZ_LUMA_FILTER_OFFSET,
    .pad_t = KVZ_LUMA_FILTER_OFFSET,
    .pad_b = KVZ_EXT_PADDING_LUMA - KVZ_LUMA_FILTER_OFFSET,
    .pad_b_simd = 1 // One row for AVX2
  };

  // Initialize separately. Gets rid of warning
  // about using nonstandard extension.
  epol_args.buf = ext_buffer;
  epol_args.ext = &ext;
  epol_args.ext_origin = &ext_origin;
  epol_args.ext_s = &ext_s;

  kvz_get_extended_block(&epol_args);
  kvz_sample_quarterpel_luma(state->encoder_control,
    ext_origin,
    ext_s,
    block_width,
    block_height,
    lcu->rec.y + (ypos % LCU_WIDTH) * LCU_WIDTH + (xpos % LCU_WIDTH),
    LCU_WIDTH,
    mv_frac_x,
    mv_frac_y,
    mv_param);
}

static void inter_recon_frac_luma_hi(const encoder_state_t *const state,
  const kvz_picture *const ref,
  int32_t xpos,
  int32_t ypos,
  int32_t block_width,
  int32_t block_height,
  const int16_t mv_param[2],
  hi_prec_buf_t *hi_prec_out)
{
  int mv_frac_x = (mv_param[0] & 3);
  int mv_frac_y = (mv_param[1] & 3);

  // Space for extrapolated pixels and the part from the picture.
  // Some extra for AVX2.
  // The extrapolation function will set the pointers and stride.
  kvz_pixel ext_buffer[KVZ_IPOL_MAX_INPUT_SIZE_LUMA_SIMD];
  kvz_pixel *ext = NULL;
  kvz_pixel *ext_origin = NULL;
  int ext_s = 0;
  kvz_epol_args epol_args = {
    .src = ref->y,
    .src_w = ref->width,
    .src_h = ref->height,
    .src_s = ref->stride,
    .blk_x = state->tile->offset_x + xpos + (mv_param[0] >> 2),
    .blk_y = state->tile->offset_y + ypos + (mv_param[1] >> 2),
    .blk_w = block_width,
    .blk_h = block_height,
    .pad_l = KVZ_LUMA_FILTER_OFFSET,
    .pad_r = KVZ_EXT_PADDING_LUMA - KVZ_LUMA_FILTER_OFFSET,
    .pad_t = KVZ_LUMA_FILTER_OFFSET,
    .pad_b = KVZ_EXT_PADDING_LUMA - KVZ_LUMA_FILTER_OFFSET,
    .pad_b_simd = 1 // One row for AVX2
  };

  // Initialize separately. Gets rid of warning
  // about using nonstandard extension.
  epol_args.buf = ext_buffer;
  epol_args.ext = &ext;
  epol_args.ext_origin = &ext_origin;
  epol_args.ext_s = &ext_s;

  kvz_get_extended_block(&epol_args);
  kvz_sample_quarterpel_luma_hi(state->encoder_control,
    ext_origin,
    ext_s,
    block_width,
    block_height,
    hi_prec_out->y + (ypos % LCU_WIDTH) * LCU_WIDTH + (xpos % LCU_WIDTH),
    LCU_WIDTH,
    mv_frac_x,
    mv_frac_y,
    mv_param);
}

static void inter_recon_frac_chroma(const encoder_state_t *const state,
  const kvz_picture *const ref,
  int32_t xpos,
  int32_t ypos,
  int32_t block_width,
  int32_t block_height,
  const int16_t mv_param[2],
  lcu_t *lcu)
{
  int mv_frac_x = (mv_param[0] & 7);
  int mv_frac_y = (mv_param[1] & 7);

  // Space for extrapolated pixels and the part from the picture.
  // Some extra for AVX2.
  // The extrapolation function will set the pointers and stride.
  kvz_pixel ext_buffer[KVZ_IPOL_MAX_INPUT_SIZE_CHROMA_SIMD];
  kvz_pixel *ext = NULL;
  kvz_pixel *ext_origin = NULL;
  int ext_s = 0;

  // Chroma U
  // Divisions by 2 due to 4:2:0 chroma subsampling
  kvz_epol_args epol_args = {
    .src = ref->u,
    .src_w = ref->width / 2,
    .src_h = ref->height / 2,
    .src_s = ref->stride / 2,
    .blk_x = (state->tile->offset_x + xpos) / 2 + (mv_param[0] >> 3),
    .blk_y = (state->tile->offset_y + ypos) / 2 + (mv_param[1] >> 3),
    .blk_w = block_width / 2,
    .blk_h = block_height / 2,
    .pad_l = KVZ_CHROMA_FILTER_OFFSET,
    .pad_r = KVZ_EXT_PADDING_CHROMA - KVZ_CHROMA_FILTER_OFFSET,
    .pad_t = KVZ_CHROMA_FILTER_OFFSET,
    .pad_b = KVZ_EXT_PADDING_CHROMA - KVZ_CHROMA_FILTER_OFFSET,
    .pad_b_simd = 3 // Three rows for AVX2
  };

  // Initialize separately. Gets rid of warning
  // about using nonstandard extension.
  epol_args.buf = ext_buffer;
  epol_args.ext = &ext;
  epol_args.ext_origin = &ext_origin;
  epol_args.ext_s = &ext_s;

  kvz_get_extended_block(&epol_args);
  kvz_sample_octpel_chroma(state->encoder_control,
    ext_origin,
    ext_s,
    block_width / 2,
    block_height / 2,
    lcu->rec.u + ((ypos / 2) % LCU_WIDTH_C) * LCU_WIDTH_C + ((xpos / 2) % LCU_WIDTH_C),
    LCU_WIDTH_C,
    mv_frac_x,
    mv_frac_y,
    mv_param);

  // Chroma V
  epol_args.src = ref->v;
  kvz_get_extended_block(&epol_args);
  kvz_sample_octpel_chroma(state->encoder_control,
    ext_origin,
    ext_s,
    block_width / 2,
    block_height / 2,
    lcu->rec.v + ((ypos / 2) % LCU_WIDTH_C) * LCU_WIDTH_C + ((xpos / 2) % LCU_WIDTH_C),
    LCU_WIDTH_C,
    mv_frac_x,
    mv_frac_y,
    mv_param);
}

static void inter_recon_frac_chroma_hi(const encoder_state_t *const state,
  const kvz_picture *const ref,
  int32_t xpos,
  int32_t ypos,
  int32_t block_width,
  int32_t block_height,
  const int16_t mv_param[2],
  hi_prec_buf_t *hi_prec_out)
{
  int mv_frac_x = (mv_param[0] & 7);
  int mv_frac_y = (mv_param[1] & 7);

  // Space for extrapolated pixels and the part from the picture.
  // Some extra for AVX2.
  // The extrapolation function will set the pointers and stride.
  kvz_pixel ext_buffer[KVZ_IPOL_MAX_INPUT_SIZE_CHROMA_SIMD];
  kvz_pixel *ext = NULL;
  kvz_pixel *ext_origin = NULL;
  int ext_s = 0;

  // Chroma U
  // Divisions by 2 due to 4:2:0 chroma subsampling
  kvz_epol_args epol_args = {
    .src = ref->u,
    .src_w = ref->width / 2,
    .src_h = ref->height / 2,
    .src_s = ref->stride / 2,
    .blk_x = (state->tile->offset_x + xpos) / 2 + (mv_param[0] >> 3),
    .blk_y = (state->tile->offset_y + ypos) / 2 + (mv_param[1] >> 3),
    .blk_w = block_width / 2,
    .blk_h = block_height / 2,
    .pad_l = KVZ_CHROMA_FILTER_OFFSET,
    .pad_r = KVZ_EXT_PADDING_CHROMA - KVZ_CHROMA_FILTER_OFFSET,
    .pad_t = KVZ_CHROMA_FILTER_OFFSET,
    .pad_b = KVZ_EXT_PADDING_CHROMA - KVZ_CHROMA_FILTER_OFFSET,
    .pad_b_simd = 3 // Three rows for AVX2
  };

  // Initialize separately. Gets rid of warning
  // about using nonstandard extension.
  epol_args.buf = ext_buffer;
  epol_args.ext = &ext;
  epol_args.ext_origin = &ext_origin;
  epol_args.ext_s = &ext_s;

  kvz_get_extended_block(&epol_args);
  kvz_sample_octpel_chroma_hi(state->encoder_control,
    ext_origin,
    ext_s,
    block_width / 2,
    block_height / 2,
    hi_prec_out->u + ((ypos / 2) % LCU_WIDTH_C) * LCU_WIDTH_C + ((xpos / 2) % LCU_WIDTH_C),
    LCU_WIDTH_C,
    mv_frac_x,
    mv_frac_y,
    mv_param);

  // Chroma V
  epol_args.src = ref->v;
  kvz_get_extended_block(&epol_args);
  kvz_sample_octpel_chroma_hi(state->encoder_control,
    ext_origin,
    ext_s,
    block_width / 2,
    block_height / 2,
    hi_prec_out->v + ((ypos / 2) % LCU_WIDTH_C) * LCU_WIDTH_C + ((xpos / 2) % LCU_WIDTH_C),
    LCU_WIDTH_C,
    mv_frac_x,
    mv_frac_y,
    mv_param);
}


/**
* \brief Copy from frame with extended border.
*
* \param ref_buf      pointer to the start of ref buffer
* \param ref_stride   stride of ref buffer
* \param ref_width    width of frame
* \param ref_height   height of frame
* \param rec_buf      pointer to the start of pu in rec buffer
* \param rec_stride   stride of rec buffer
* \param width        width of copied block
* \param height       height of copied block
* \param mv_in_frame  coordinates of copied block in frame coordinates
*/
static void inter_cp_with_ext_border(const kvz_pixel *ref_buf, int ref_stride,
                                     int ref_width, int ref_height,
                                     kvz_pixel *rec_buf, int rec_stride,
                                     int width, int height,
                                     const vector2d_t *mv_in_frame)
{
  for (int y = mv_in_frame->y; y < mv_in_frame->y + height; ++y) {
    for (int x = mv_in_frame->x; x < mv_in_frame->x + width; ++x) {
      vector2d_t in_frame = {
        CLIP(0, ref_width - 1, x),
        CLIP(0, ref_height - 1, y),
      };
      vector2d_t in_pu = {
        x - mv_in_frame->x,
        y - mv_in_frame->y,
      };
      int pu_index = in_pu.y * rec_stride + in_pu.x;
      int frame_index = in_frame.y * ref_stride + in_frame.x;
      rec_buf[pu_index] = ref_buf[frame_index];
    }
  }
}


/**
 * \brief Reconstruct an inter PU using uniprediction.
 *
 * \param state          encoder state
 * \param ref            picture to copy the data from
 * \param xpos           PU x position
 * \param ypos           PU y position
 * \param width          PU width
 * \param height         PU height
 * \param mv_param       motion vector
 * \param lcu            destination lcu
 * \param hi_prec_out    destination of high precision output, or NULL if not needed
 * \param predict_luma   Enable or disable luma prediction for this call.
 * \param predict_chroma Enable or disable chroma prediction for this call.
*/
static void inter_recon_unipred(const encoder_state_t * const state,
                                const kvz_picture * const ref,
                                int32_t xpos,
                                int32_t ypos,
                                int32_t width,
                                int32_t height,
                                const mv_t mv_param[2],
                                lcu_t *lcu,
                                hi_prec_buf_t *hi_prec_out,
                                bool predict_luma,
                                bool predict_chroma)
{
  mv_t mv_param_qpel[2] = { mv_param[0], mv_param[1] };
  mv_t mv_param_fpel[2] = { mv_param[0], mv_param[1] };
  kvz_change_precision(INTERNAL_MV_PREC, 2, &mv_param_qpel[0], &mv_param_qpel[1]);

  kvz_change_precision(INTERNAL_MV_PREC, 0, &mv_param_fpel[0], &mv_param_fpel[1]);

  const vector2d_t pu_in_tile = { xpos, ypos };
  const vector2d_t pu_in_lcu = { xpos % LCU_WIDTH, ypos % LCU_WIDTH };

  const vector2d_t mv_in_pu = { mv_param_fpel[0], mv_param_fpel[1] };
  const vector2d_t mv_in_frame = {
    mv_in_pu.x + pu_in_tile.x + state->tile->offset_x,
    mv_in_pu.y + pu_in_tile.y + state->tile->offset_y
  };

  const bool mv_is_outside_frame = mv_in_frame.x < 0 ||
      mv_in_frame.y < 0 ||
      mv_in_frame.x + width > ref->width ||
      mv_in_frame.y + height > ref->height;

  // With 420, odd coordinates need interpolation.
  const int8_t fractional_chroma = (mv_in_pu.x & 1) || (mv_in_pu.y & 1);
  const int8_t fractional_luma = ((mv_param_qpel[0] & 3) || (mv_param_qpel[1] & 3));

  // Generate prediction for luma.
  if (predict_luma) {
    if (fractional_luma) {
      // With a fractional MV, do interpolation.
      if (state->encoder_control->cfg.bipred && hi_prec_out) {
        inter_recon_frac_luma_hi(state, ref,
          pu_in_tile.x, pu_in_tile.y,
          width, height,
          mv_param_qpel, hi_prec_out);
      }
      else {
        inter_recon_frac_luma(state, ref,
          pu_in_tile.x, pu_in_tile.y,
          width, height,
          mv_param_qpel, lcu);
      }
    } else {
      // With an integer MV, copy pixels directly from the reference.
      const int lcu_pu_index = pu_in_lcu.y * LCU_WIDTH + pu_in_lcu.x;
      if (mv_is_outside_frame) {
        inter_cp_with_ext_border(ref->y, ref->stride,
                                 ref->width, ref->height,
                                 &lcu->rec.y[lcu_pu_index], LCU_WIDTH,
                                 width, height,
                                 &mv_in_frame);
      }
      else {
        const int frame_mv_index = mv_in_frame.y * ref->stride + mv_in_frame.x;
        kvz_pixels_blit(&ref->y[frame_mv_index],
          &lcu->rec.y[lcu_pu_index],
          width, height,
          ref->stride, LCU_WIDTH);
      }
    }
  }

  if (!predict_chroma) {
    return;
  }

  // Generate prediction for chroma.
  if (fractional_luma || fractional_chroma) {
    // With a fractional MV, do interpolation.
    if (state->encoder_control->cfg.bipred && hi_prec_out) {
      inter_recon_frac_chroma_hi(state, ref,
                                    pu_in_tile.x, pu_in_tile.y,
                                    width, height,
                                    mv_param_qpel, hi_prec_out);
    } else {
      inter_recon_frac_chroma(state, ref,
                              pu_in_tile.x, pu_in_tile.y,
                              width, height,
                              mv_param_qpel, lcu);
    }
  } else {
    // With an integer MV, copy pixels directly from the reference.
    const int lcu_pu_index_c = pu_in_lcu.y / 2 * LCU_WIDTH_C + pu_in_lcu.x / 2;
    const vector2d_t mv_in_frame_c = { mv_in_frame.x / 2, mv_in_frame.y / 2 };

    if (mv_is_outside_frame) {
      inter_cp_with_ext_border(ref->u, ref->stride / 2,
                               ref->width / 2, ref->height / 2,
                               &lcu->rec.u[lcu_pu_index_c], LCU_WIDTH_C,
                               width / 2, height / 2,
                               &mv_in_frame_c);
      inter_cp_with_ext_border(ref->v, ref->stride / 2,
                               ref->width / 2, ref->height / 2,
                               &lcu->rec.v[lcu_pu_index_c], LCU_WIDTH_C,
                               width / 2, height / 2,
                               &mv_in_frame_c);
    } else {
      const int frame_mv_index = mv_in_frame_c.y * ref->stride / 2 + mv_in_frame_c.x;

      kvz_pixels_blit(&ref->u[frame_mv_index],
                      &lcu->rec.u[lcu_pu_index_c],
                      width / 2, height / 2,
                      ref->stride / 2, LCU_WIDTH_C);
      kvz_pixels_blit(&ref->v[frame_mv_index],
                      &lcu->rec.v[lcu_pu_index_c],
                      width / 2, height / 2,
                      ref->stride / 2, LCU_WIDTH_C);
    }
  }
}
/**
 * \brief Reconstruct bi-pred inter PU
 *
 * \param state          encoder state
 * \param ref1           reference picture to copy the data from
 * \param ref2           other reference picture to copy the data from
 * \param xpos           PU x position
 * \param ypos           PU y position
 * \param width          PU width
 * \param height         PU height
 * \param mv_param       motion vectors
 * \param lcu            destination lcu
 * \param predict_luma   Enable or disable luma prediction for this call.
 * \param predict_chroma Enable or disable chroma prediction for this call.
 */
void kvz_inter_recon_bipred(const encoder_state_t * const state,
                            const kvz_picture * ref1,
                            const kvz_picture * ref2,
                            int32_t xpos,
                            int32_t ypos,
                            int32_t width,
                            int32_t height,
                            mv_t mv_param[2][2],
                            lcu_t* lcu,
                            bool predict_luma,
                            bool predict_chroma)
{
  kvz_pixel temp_lcu_y[LCU_WIDTH*LCU_WIDTH];
  kvz_pixel temp_lcu_u[LCU_WIDTH_C*LCU_WIDTH_C];
  kvz_pixel temp_lcu_v[LCU_WIDTH_C*LCU_WIDTH_C];

  const int hi_prec_luma_rec0 = (mv_param[0][0]>>2) & 3 || (mv_param[0][1]>>2) & 3;
  const int hi_prec_luma_rec1 = (mv_param[1][0]>>2) & 3 || (mv_param[1][1]>>2) & 3;

  const int hi_prec_chroma_rec0 = (mv_param[0][0]>>2) & 7 || (mv_param[0][1]>>2) & 7;
  const int hi_prec_chroma_rec1 = (mv_param[1][0]>>2) & 7 || (mv_param[1][1]>>2) & 7;

  hi_prec_buf_t* high_precision_rec0 = 0;
  hi_prec_buf_t* high_precision_rec1 = 0;
  if (hi_prec_chroma_rec0) high_precision_rec0 = kvz_hi_prec_buf_t_alloc(LCU_WIDTH*LCU_WIDTH);
  if (hi_prec_chroma_rec1) high_precision_rec1 = kvz_hi_prec_buf_t_alloc(LCU_WIDTH*LCU_WIDTH);


  //Reconstruct both predictors
  inter_recon_unipred(state, ref1, xpos, ypos, width, height, mv_param[0], lcu, high_precision_rec0,
                      predict_luma, predict_chroma);
  if (!hi_prec_luma_rec0){
    memcpy(temp_lcu_y, lcu->rec.y, sizeof(kvz_pixel) * 64 * 64); // copy to temp_lcu_y
  }
  if (!hi_prec_chroma_rec0){
    memcpy(temp_lcu_u, lcu->rec.u, sizeof(kvz_pixel) * 32 * 32); // copy to temp_lcu_u
    memcpy(temp_lcu_v, lcu->rec.v, sizeof(kvz_pixel) * 32 * 32); // copy to temp_lcu_v
  }
  inter_recon_unipred(state, ref2, xpos, ypos, width, height, mv_param[1], lcu, high_precision_rec1,
                      predict_luma, predict_chroma);

  // After reconstruction, merge the predictors by taking an average of each pixel
  kvz_inter_recon_bipred_blend(hi_prec_luma_rec0, hi_prec_luma_rec1, 
                               hi_prec_chroma_rec0, hi_prec_chroma_rec1,
                               height, width, ypos, xpos,
                               high_precision_rec0, high_precision_rec1,
                               lcu, temp_lcu_y, temp_lcu_u, temp_lcu_v, predict_luma, predict_chroma);
 
  if (high_precision_rec0 != 0) kvz_hi_prec_buf_t_free(high_precision_rec0);
  if (high_precision_rec1 != 0) kvz_hi_prec_buf_t_free(high_precision_rec1);
}


/**
 * Reconstruct a single CU.
 *
 * The CU may consist of multiple PUs, each of which can use either
 * uniprediction or biprediction.
 *
 * \param state   encoder state
 * \param lcu     containing LCU
 * \param x       x-coordinate of the CU in pixels
 * \param y       y-coordinate of the CU in pixels
 * \param width   CU width
 * \param predict_luma   Enable or disable luma prediction for this call.
 * \param predict_chroma Enable or disable chroma prediction for this call.
 */
void kvz_inter_recon_cu(const encoder_state_t * const state,
                        lcu_t *lcu,
                        int32_t x,
                        int32_t y,
                        int32_t width,
                        bool predict_luma,
                        bool predict_chroma)
{
  cu_info_t *cu = LCU_GET_CU_AT_PX(lcu, SUB_SCU(x), SUB_SCU(y));
  const int num_pu = kvz_part_mode_num_parts[cu->part_size];
  for (int i = 0; i < num_pu; ++i) {
    kvz_inter_pred_pu(state, lcu, x, y, width, predict_luma, predict_chroma, i);
  }
}

/**
 * Predict a single PU.
 *
 * The PU may use either uniprediction or biprediction.
 *
 * \param state          encoder state
 * \param lcu            containing LCU
 * \param x              x-coordinate of the CU in pixels
 * \param y              y-coordinate of the CU in pixels
 * \param width          CU width
 * \param predict_luma   Enable or disable luma prediction for this call.
 * \param predict_chroma Enable or disable chroma prediction for this call.
 * \param i_pu           Index of the PU. Always zero for 2Nx2N. Used for SMP+AMP.
 */
void kvz_inter_pred_pu(const encoder_state_t * const state,
                       lcu_t *lcu,
                       int32_t x,
                       int32_t y,
                       int32_t width,
                       bool predict_luma,
                       bool predict_chroma,
                       int i_pu)

{
  cu_info_t *cu = LCU_GET_CU_AT_PX(lcu, SUB_SCU(x), SUB_SCU(y));
  const int pu_x = PU_GET_X(cu->part_size, width, x, i_pu);
  const int pu_y = PU_GET_Y(cu->part_size, width, y, i_pu);
  const int pu_w = PU_GET_W(cu->part_size, width, i_pu);
  const int pu_h = PU_GET_H(cu->part_size, width, i_pu);
  cu_info_t *pu = LCU_GET_CU_AT_PX(lcu, SUB_SCU(pu_x), SUB_SCU(pu_y));

  if (pu->inter.mv_dir == 3) {
    const kvz_picture *const refs[2] = {
      state->frame->ref->images[
        state->frame->ref_LX[0][
          pu->inter.mv_ref[0]]],
      state->frame->ref->images[
        state->frame->ref_LX[1][
          pu->inter.mv_ref[1]]],
    };
    kvz_inter_recon_bipred(state,
      refs[0], refs[1],
      pu_x, pu_y,
      pu_w, pu_h,
      pu->inter.mv,
      lcu,
      predict_luma, predict_chroma);
  }
  else {
    const int mv_idx = pu->inter.mv_dir - 1;
    const kvz_picture *const ref =
      state->frame->ref->images[
        state->frame->ref_LX[mv_idx][
          pu->inter.mv_ref[mv_idx]]];

    inter_recon_unipred(state,
      ref,
      pu_x, pu_y,
      pu_w, pu_h,
      pu->inter.mv[mv_idx],
      lcu,
      NULL,
      predict_luma, predict_chroma);
  }
}

/**
 * \brief Clear unused L0/L1 motion vectors and reference
 * \param cu coding unit to clear
 */
static void inter_clear_cu_unused(cu_info_t* cu)
{
  for (unsigned i = 0; i < 2; ++i) {
    if (cu->inter.mv_dir & (1 << i)) continue;

    cu->inter.mv[i][0] = 0;
    cu->inter.mv[i][1] = 0;
    cu->inter.mv_ref[i] = 255;
  }
}

/**
 * \brief Check whether a0 mv cand block is coded before the current block.
 * \param x       x-coordinate of the current block (in pixels)
 * \param y       y-coordinate of the current block (in pixels)
 * \param width   width of the current block (in pixels)
 * \param height  height of the current block (in pixels)
 * \return        True, if the a0 mv candidate block is coded before the
 *                current block. Otherwise false.
 */
static bool is_a0_cand_coded(int x, int y, int width, int height)
{
  int size = MIN(width & ~(width - 1), height & ~(height - 1));

  if (height != size) {
    // For SMP and AMP blocks the situation is equivalent to a square block
    // at the lower left corner of the PU.
    y = y + height - size;
  }

  while (size < LCU_WIDTH) {
    const int parent_size = 2 * size;
    const int cu_index    = (x % parent_size != 0) + 2 * (y % parent_size != 0);
    switch (cu_index) {
      case 0:
        // A0 is in the CU directly left of the parent CU so it has been
        // coded already.
        //    +---+---+
        //    | X |   |
        //    |---+---+
        // A0 |   |   |
        //    +---+---+
        return true;

      case 1:
        // A0 is in the CU that will be coded after the current CU.
        //    +---+---+
        //    |   | X |
        //    |---+---+
        //    |A0 |   |
        //    +---+---+
        return false;

      case 2:
        //    +---+---+
        //    |   |   |
        //    |---+---+
        //    | X |   |
        //    +---+---+
        // A0

        // Move to the parent block.
        y -= size;
        size = parent_size;
        break;

      case 3:
        // A0 is in the CU directly down of the parent CU so is has not
        // been coded yet.
        //    +---+---+
        //    |   |   |
        //    |---+---+
        //    |   | X |
        //    +---+---+
        //     A0
        return false;
    }
  }

  // For 64x64 blocks A0 candidate is located outside the LCU.
  return false;
}

/**
 * \brief Check whether b0 mv cand block is coded before the current block.
 * \param x       x-coordinate of the current block (in pixels)
 * \param y       y-coordinate of the current block (in pixels)
 * \param width   width of the current block (in pixels)
 * \param height  height of the current block (in pixels)
 * \return        True, if the b0 mv candidate block is coded before the
 *                current block. Otherwise false.
 */
static bool is_b0_cand_coded(int x, int y, int width, int height)
{
  int size = MIN(width & ~(width - 1), height & ~(height - 1));

  if (width != size) {
    // For SMP and AMP blocks the situation is equivalent to a square block
    // at the upper right corner of the PU.
    x = x + width - size;
  }

  while (size < LCU_WIDTH) {
    const int parent_size = 2 * size;
    const int cu_index    = (x % parent_size != 0) + 2 * (y % parent_size != 0);
    switch (cu_index) {
      case 0:
        // B0 is in the CU directly above the parent CU so it has been
        // coded already.
        //         B0
        //    +---+---+
        //    | X |   |
        //    |---+---+
        //    |   |   |
        //    +---+---+
        return true;

      case 1:
        //             B0
        //    +---+---+
        //    |   | X |
        //    |---+---+
        //    |   |   |
        //    +---+---+

        // Move to the parent block.
        x -= size;
        size = parent_size;
        break;

      case 2:
        //    +---+---+
        //    |   |B0 |
        //    |---+---+
        //    | X |   |
        //    +---+---+
        return true;

      case 3:
        // B0 is in the CU directly right of the parent CU so is has not
        // been coded yet.
        //    +---+---+
        //    |   |   | B0
        //    |---+---+
        //    |   | X |
        //    +---+---+
        return false;
    }
  }

  // The LCU to the right and up of the current LCU has been coded already.
  return true;
}


/**
 * \brief Get merge candidates for current block
 *
 * \param state     encoder control state to use
 * \param x         block x position in SCU
 * \param y         block y position in SCU
 * \param width     current block width
 * \param height    current block height
 * \param ref_list  which reference list, L0 is 1 and L1 is 2
 * \param ref_idx   index in the reference list
 * \param cand_out  will be filled with C3 and H candidates
 */
static void get_temporal_merge_candidates(const encoder_state_t * const state,
                                          int32_t x,
                                          int32_t y,
                                          int32_t width,
                                          int32_t height,
                                          uint8_t ref_list,
                                          uint8_t ref_idx,
                                          merge_candidates_t *cand_out)
{
  /*
  Predictor block locations
  _________
  |CurrentPU|
  | |C0|__  |
  |    |C3| |
  |_________|_
            |H|
  */

  cand_out->c3 = cand_out->h = NULL;

  // Find temporal reference
  if (state->frame->ref->used_size) {
    uint32_t colocated_ref;

    // Select L0/L1 ref_idx reference
    if (state->frame->ref_LX_size[ref_list-1] > ref_idx) {
      colocated_ref = state->frame->ref_LX[ref_list - 1][ref_idx];
    } else {
      // not found
      return;
    }

    cu_array_t *ref_cu_array = state->frame->ref->cu_arrays[colocated_ref];
    int cu_per_width = ref_cu_array->width / SCU_WIDTH;

    uint32_t xColBr = x + width;
    uint32_t yColBr = y + height;

    // H must be available
    if (xColBr < state->encoder_control->in.width &&
        yColBr < state->encoder_control->in.height) {
      int32_t H_offset = -1;

      // Y inside the current CTU / LCU
      if (yColBr % LCU_WIDTH != 0) {
        H_offset = ((xColBr >> 3) << 3) / SCU_WIDTH +
                  (((yColBr >> 3) << 3) / SCU_WIDTH) * cu_per_width;
      }

      if (H_offset >= 0) {
        // Only use when it's inter block
        if (ref_cu_array->data[H_offset].type == CU_INTER) {
          cand_out->h = &ref_cu_array->data[H_offset];
        }
      }
    }
    uint32_t xColCtr = x + (width / 2);
    uint32_t yColCtr = y + (height / 2);

    // C3 must be inside the LCU, in the center position of current CU
    if (xColCtr < state->encoder_control->in.width && yColCtr < state->encoder_control->in.height) {
      uint32_t C3_offset = ((xColCtr >> 3) << 3) / SCU_WIDTH + ((((yColCtr >> 3) << 3) / SCU_WIDTH) * cu_per_width);
      if (ref_cu_array->data[C3_offset].type == CU_INTER) {
        cand_out->c3 = &ref_cu_array->data[C3_offset];
      }
    }
  }
}

/**
 * \brief Get merge candidates for current block.
 *
 * The output parameters b0, b1, b2, a0, a1 are pointed to the
 * corresponding cu_info_t struct in lcu->cu, or set to NULL, if the
 * candidate is not available.
 *
 * \param x               block x position in pixels
 * \param y               block y position in pixels
 * \param width           block width in pixels
 * \param height          block height in pixels
 * \param picture_width   tile width in pixels
 * \param picture_height  tile height in pixels
 * \param lcu             current LCU
 * \param cand_out        will be filled with A and B candidates
 */
static void get_spatial_merge_candidates(int32_t x,
                                         int32_t y,
                                         int32_t width,
                                         int32_t height,
                                         int32_t picture_width,
                                         int32_t picture_height,
                                         lcu_t *lcu,
                                         merge_candidates_t *cand_out,
                                         uint8_t parallel_merge_level,
                                         bool wpp
  )
{
  /*
  Predictor block locations
  ____      _______
  |B2|______|B1|B0|
     |         |
     |  Cur CU |
   __|         |
  |A1|_________|
  |A0|
  */
  int32_t x_local = SUB_SCU(x); //!< coordinates from top-left of this LCU
  int32_t y_local = SUB_SCU(y);
  // A0 and A1 availability testing
  if (x != 0) {
    cu_info_t *a1 = LCU_GET_CU_AT_PX(lcu, x_local - 1, y_local + height - 1);
    // Do not check a1->coded because the block above is always coded before
    // the current one and the flag is not set when searching an SMP block.
    if (a1->type == CU_INTER) {
      inter_clear_cu_unused(a1);
      cand_out->a[1] = a1;
      cand_out->mer_a1[0] = parallel_merge_level;
    }

    if (y_local + height < LCU_WIDTH && y + height < picture_height) {
      cu_info_t *a0 = LCU_GET_CU_AT_PX(lcu, x_local - 1, y_local + height);
      if (a0->type == CU_INTER && is_a0_cand_coded(x, y, width, height)) {
        inter_clear_cu_unused(a0);
        cand_out->a[0] = a0;
      }
    }
  }

  // B0, B1 and B2 availability testing
  if (y != 0) {
    cu_info_t *b0 = NULL;
    if (x + width < picture_width) {
      if (x_local + width < LCU_WIDTH) {
        b0 = LCU_GET_CU_AT_PX(lcu, x_local + width, y_local - 1);
      } else if (!wpp && y_local == 0) {
        // Special case, top-right CU
        b0 = LCU_GET_TOP_RIGHT_CU(lcu);
      }
    }
    if (b0 && b0->type == CU_INTER && is_b0_cand_coded(x, y, width, height)) {
      inter_clear_cu_unused(b0);
      cand_out->b[0] = b0;
    }

    cu_info_t *b1 = LCU_GET_CU_AT_PX(lcu, x_local + width - 1, y_local - 1);
    // Do not check b1->coded because the block to the left is always coded
    // before the current one and the flag is not set when searching an SMP
    // block.
    if (b1->type == CU_INTER) {
      inter_clear_cu_unused(b1);
      cand_out->b[1] = b1;
    }

    if (x != 0) {
      cu_info_t *b2 = LCU_GET_CU_AT_PX(lcu, x_local - 1, y_local - 1);
      // Do not check b2->coded because the block above and to the left is
      // always coded before the current one.
      if (b2->type == CU_INTER) {
        inter_clear_cu_unused(b2);
        cand_out->b[2] = b2;
      }
    }
  }
}

/**
 * \brief Get merge candidates for current block.
 *
 * The output parameters b0, b1, b2, a0, a1 are pointed to the
 * corresponding cu_info_t struct in lcu->cu, or set to NULL, if the
 * candidate is not available.
 *
 * \param cua             cu information
 * \param x               block x position in pixels
 * \param y               block y position in pixels
 * \param width           block width in pixels
 * \param height          block height in pixels
 * \param picture_width   tile width in pixels
 * \param picture_height  tile height in pixels
 * \param cand_out        will be filled with A and B candidates
 */
static void get_spatial_merge_candidates_cua(const cu_array_t *cua,
                                             int32_t x,
                                             int32_t y,
                                             int32_t width,
                                             int32_t height,
                                             int32_t picture_width,
                                             int32_t picture_height,
                                             merge_candidates_t *cand_out,
                                             bool wpp)
{
  /*
  Predictor block locations
  ____      _______
  |B2|______|B1|B0|
     |         |
     |  Cur CU |
   __|         |
  |A1|_________|
  |A0|
  */
  int32_t x_local = SUB_SCU(x); //!< coordinates from top-left of this LCU
  int32_t y_local = SUB_SCU(y);
  // A0 and A1 availability testing
  if (x != 0) {
    const cu_info_t *a1 = kvz_cu_array_at_const(cua, x - 1, y + height - 1);
    // The block above is always coded before the current one.
    if (a1->type == CU_INTER) {
      cand_out->a[1] = a1;
    }

    if (y_local + height < LCU_WIDTH && y + height < picture_height) {
      const cu_info_t *a0 = kvz_cu_array_at_const(cua, x - 1, y + height);
      if (a0->type == CU_INTER && is_a0_cand_coded(x, y, width, height)) {
        cand_out->a[0] = a0;
      }
    }
  }

  // B0, B1 and B2 availability testing
  if (y != 0) {
    if (x + width < picture_width && (x_local + width < LCU_WIDTH || (!wpp && y_local == 0))) {
      const cu_info_t *b0 = kvz_cu_array_at_const(cua, x + width, y - 1);
      if (b0->type == CU_INTER && is_b0_cand_coded(x, y, width, height)) {
        cand_out->b[0] = b0;
      }
    }

    const cu_info_t* b1 = kvz_cu_array_at_const(cua, x + width - 1, y - 1);
    // The block to the left is always coded before the current one.
    if (b1->type == CU_INTER) {
      cand_out->b[1] = b1;
    }

    if (x != 0) {
      const cu_info_t *b2 = kvz_cu_array_at_const(cua, x - 1, y - 1);
      // The block above and to the left is always coded before the current
      // one.
      if (b2->type == CU_INTER) {
        cand_out->b[2] = b2;
      }
    }
  }
}

static INLINE int16_t get_scaled_mv(int16_t mv, int scale)
{
  int32_t scaled = scale * mv;
  return CLIP(-32768, 32767, (scaled + 127 + (scaled < 0)) >> 8);
}

static void apply_mv_scaling_pocs(int32_t current_poc,
                                  int32_t current_ref_poc,
                                  int32_t neighbor_poc,
                                  int32_t neighbor_ref_poc,
                                  int16_t mv_cand[2])
{
  int32_t diff_current  = current_poc  - current_ref_poc;
  int32_t diff_neighbor = neighbor_poc - neighbor_ref_poc;

  if (diff_current == diff_neighbor) return;

  diff_current  = CLIP(-128, 127, diff_current);
  diff_neighbor = CLIP(-128, 127, diff_neighbor);

  int scale = CLIP(-4096, 4095,
    (diff_current * ((0x4000 + (abs(diff_neighbor) >> 1)) / diff_neighbor) + 32) >> 6);

  mv_cand[0] = get_scaled_mv(mv_cand[0], scale);
  mv_cand[1] = get_scaled_mv(mv_cand[1], scale);
}

static INLINE void apply_mv_scaling(const encoder_state_t *state,
                                    const cu_info_t *current_cu,
                                    const cu_info_t *neighbor_cu,
                                    int8_t current_reflist,
                                    int8_t neighbor_reflist,
                                    int16_t mv_cand[2])
{
  apply_mv_scaling_pocs(state->frame->poc,
                        state->frame->ref->pocs[
                          state->frame->ref_LX[current_reflist][
                            current_cu->inter.mv_ref[current_reflist]]],
                        state->frame->poc,
                        state->frame->ref->pocs[
                          state->frame->ref_LX[neighbor_reflist][
                            neighbor_cu->inter.mv_ref[neighbor_reflist]]],
                        mv_cand);
}

/**
 * \brief Try to add a temporal MVP or merge candidate.
 *
 * \param state         encoder state
 * \param current_ref   index of the picture referenced by the current CU
 * \param colocated     colocated CU
 * \param reflist       either 0 (for L0) or 1 (for L1)
 * \param[out] mv_out   Returns the motion vector
 *
 * \return Whether a temporal candidate was added or not.
 */
static bool add_temporal_candidate(const encoder_state_t *state,
                                   uint8_t current_ref,
                                   const cu_info_t *colocated,
                                   int32_t reflist,
                                   int16_t mv_out[2])
{
  if (!colocated) return false;

  int colocated_ref;
  if (state->frame->ref_LX_size[0] > 0) {
    // get the first reference from L0 if it exists
    colocated_ref = state->frame->ref_LX[0][0];
  } else {
    // otherwise no candidate added
    return false;
  }

  // When there are reference pictures from the future (POC > current POC)
  // in L0 or L1, the primary list for the colocated PU is the inverse of
  // collocated_from_l0_flag. Otherwise it is equal to reflist.
  //
  // Kvazaar always sets collocated_from_l0_flag so the list is L1 when
  // there are future references.
  int col_list = reflist;
  /*for (int i = 0; i < state->frame->ref->used_size; i++) {
    if (state->frame->ref->pocs[i] > state->frame->poc) {
      col_list = 1;
      break;
    }
  }*/
  
  if ((colocated->inter.mv_dir & (col_list + 1)) == 0) {
    // Use the other list if the colocated PU does not have a MV for the
    // primary list.
    col_list = 1 - col_list;
  }

  mv_out[0] = colocated->inter.mv[col_list][0];
  mv_out[1] = colocated->inter.mv[col_list][1];
  apply_mv_scaling_pocs(
    state->frame->poc,
    state->frame->ref->pocs[current_ref],
    state->frame->ref->pocs[colocated_ref],
    state->frame->ref->images[colocated_ref]->ref_pocs[
      state->frame->ref->ref_LXs[colocated_ref]
        [col_list][colocated->inter.mv_ref[col_list]]],
    mv_out
  );

  return true;
}

static INLINE bool add_mvp_candidate(const encoder_state_t *state,
                                     const cu_info_t *cur_cu,
                                     const cu_info_t *cand,
                                     int8_t reflist,
                                     bool scaling,
                                     int16_t mv_cand_out[2])
{
  if (!cand) return false;

  assert(cand->inter.mv_dir != 0);

  for (int i = 0; i < 2; i++) {
    const int cand_list = i == 0 ? reflist : !reflist;

    if ((cand->inter.mv_dir & (1 << cand_list)) == 0) continue;

    if (scaling) {
      mv_cand_out[0] = cand->inter.mv[cand_list][0];
      mv_cand_out[1] = cand->inter.mv[cand_list][1];
      apply_mv_scaling(state, cur_cu, cand, reflist, cand_list, mv_cand_out);
      return true;
    }

    if (state->frame->ref_LX[cand_list][cand->inter.mv_ref[cand_list]] ==
        state->frame->ref_LX[reflist][cur_cu->inter.mv_ref[reflist]])
    {
      mv_cand_out[0] = cand->inter.mv[cand_list][0];
      mv_cand_out[1] = cand->inter.mv[cand_list][1];
      return true;
    }
  }

  return false;
}

/**
 * \brief Pick two mv candidates from the spatial and temporal candidates.
 */
static void get_mv_cand_from_candidates(const encoder_state_t * const state,
                                        int32_t x,
                                        int32_t y,
                                        int32_t width,
                                        int32_t height,
                                        const merge_candidates_t *merge_cand,
                                        const cu_info_t *cur_cu,
                                        int8_t reflist,
                                        int16_t mv_cand[2][2])
{
  const cu_info_t *const *a = merge_cand->a;
  const cu_info_t *const *b = merge_cand->b;
  const cu_info_t *c3 = merge_cand->c3;
  const cu_info_t *h  = merge_cand->h;

  uint8_t candidates = 0;
  uint8_t b_candidates = 0;

  // Left predictors without scaling
  if (add_mvp_candidate(state, cur_cu, a[0], reflist, false, mv_cand[candidates])) {
    candidates++;
  } else if (add_mvp_candidate(state, cur_cu, a[1], reflist, false, mv_cand[candidates])) {
    candidates++;
  }


  // Top predictors without scaling  
  if (add_mvp_candidate(state, cur_cu, b[0], reflist, false, mv_cand[candidates])) {
    b_candidates++;
  } else if (add_mvp_candidate(state, cur_cu, b[1], reflist, false, mv_cand[candidates])) {
    b_candidates++;
  }
  else if (add_mvp_candidate(state, cur_cu, b[2], reflist, false, mv_cand[candidates])) {
    b_candidates++;
  }

  candidates += b_candidates;

  // Remove identical candidate
  if (candidates == 2 && mv_cand[0][0] == mv_cand[1][0] && mv_cand[0][1] == mv_cand[1][1]) {
    candidates = 1;
  }

  // Use Temporal Motion Vector Prediction when enabled.
  // TMVP required at least two sequential P/B-frames.
  bool can_use_tmvp =
    state->encoder_control->cfg.tmvp_enable &&
    state->frame->poc > 1 &&
    state->frame->ref->used_size &&
    candidates < AMVP_MAX_NUM_CANDS &&
    (h != NULL || c3 != NULL);

  if (can_use_tmvp && add_temporal_candidate(state,
                                             state->frame->ref_LX[reflist][cur_cu->inter.mv_ref[reflist]],
                                             (h != NULL) ? h : c3,
                                             reflist,
                                             mv_cand[candidates]))
  {
    candidates++;
  }

  if (candidates < AMVP_MAX_NUM_CANDS)
  {
    const uint32_t ctu_row = (y >> LOG2_LCU_WIDTH);
    const uint32_t ctu_row_mul_five = ctu_row * MAX_NUM_HMVP_CANDS;
    int32_t num_cand = state->frame->hmvp_size[ctu_row];
    for (int i = 0; i < MIN(/*MAX_NUM_HMVP_AVMPCANDS*/4,num_cand); i++) {
      for (int pred_source = 0; pred_source < 2; pred_source++) {
        const int cand_list = pred_source == 0 ? reflist : !reflist;
        cu_info_t* cand = &state->frame->hmvp_lut[ctu_row_mul_five + num_cand - 1 - i];
        if ((cand->inter.mv_dir & (1 << cand_list)) == 0) continue;
        // Make sure the candidate points to the same reference frame
        if (state->frame->ref_LX[cand_list][cand->inter.mv_ref[cand_list]] ==
            state->frame->ref_LX[reflist][cur_cu->inter.mv_ref[reflist]])
        {
          mv_cand[candidates][0] = cand->inter.mv[cand_list][0];
          mv_cand[candidates][1] = cand->inter.mv[cand_list][1];
          candidates++;
          if (candidates == AMVP_MAX_NUM_CANDS) return;
        }
      }
    }
  }


  // Fill with (0,0)
  while (candidates < AMVP_MAX_NUM_CANDS) {
    mv_cand[candidates][0] = 0;
    mv_cand[candidates][1] = 0;
    candidates++;
  }
}

/**
 * \brief Get MV prediction for current block.
 *
 * \param state     encoder state
 * \param x         block x position in pixels
 * \param y         block y position in pixels
 * \param width     block width in pixels
 * \param height    block height in pixels
 * \param mv_cand   Return the motion vector candidates.
 * \param cur_cu    current CU
 * \param lcu       current LCU
 * \param reflist   reflist index (either 0 or 1)
 */
void kvz_inter_get_mv_cand(const encoder_state_t * const state,
                           int32_t x,
                           int32_t y,
                           int32_t width,
                           int32_t height,
                           mv_t mv_cand[2][2],
                           cu_info_t* cur_cu,
                           lcu_t *lcu,
                           int8_t reflist)
{
  merge_candidates_t merge_cand = { 0 };
  const uint8_t parallel_merge_level = state->encoder_control->cfg.log2_parallel_merge_level;
  get_spatial_merge_candidates(x, y, width, height,
                               state->tile->frame->width,
                               state->tile->frame->height,
                               lcu,
                               &merge_cand, parallel_merge_level,state->encoder_control->cfg.wpp);
  get_temporal_merge_candidates(state, x, y, width, height, 1, 0, &merge_cand);
  get_mv_cand_from_candidates(state, x, y, width, height, &merge_cand, cur_cu, reflist, mv_cand);
    
  kvz_round_precision(INTERNAL_MV_PREC, 2, &mv_cand[0][0], &mv_cand[0][1]);
  kvz_round_precision(INTERNAL_MV_PREC, 2, &mv_cand[1][0], &mv_cand[1][1]);
}

/**
 * \brief Get MV prediction for current block using state->tile->frame->cu_array.
 *
 * \param state     encoder state
 * \param x         block x position in pixels
 * \param y         block y position in pixels
 * \param width     block width in pixels
 * \param height    block height in pixels
 * \param mv_cand   Return the motion vector candidates.
 * \param cur_cu    current CU
 * \param reflist   reflist index (either 0 or 1)
 */
void kvz_inter_get_mv_cand_cua(const encoder_state_t * const state,
                               int32_t x,
                               int32_t y,
                               int32_t width,
                               int32_t height,
                               mv_t mv_cand[2][2],
                               const cu_info_t* cur_cu,
                               int8_t reflist)
{
  merge_candidates_t merge_cand = { 0 };

  const cu_array_t *cua = state->tile->frame->cu_array;
  get_spatial_merge_candidates_cua(cua,
                                   x, y, width, height,
                                   state->tile->frame->width, state->tile->frame->height,
                                   &merge_cand, state->encoder_control->cfg.wpp);
  get_temporal_merge_candidates(state, x, y, width, height, 1, 0, &merge_cand);
  get_mv_cand_from_candidates(state, x, y, width, height, &merge_cand, cur_cu, reflist, mv_cand);

  kvz_round_precision(INTERNAL_MV_PREC, 2, &mv_cand[0][0], &mv_cand[0][1]);
  kvz_round_precision(INTERNAL_MV_PREC, 2, &mv_cand[1][0], &mv_cand[1][1]);
}

static bool is_duplicate_candidate(const cu_info_t* cu1, const cu_info_t* cu2)
{
  if (!cu2) return false;
  if (cu1->inter.mv_dir != cu2->inter.mv_dir) return false;

  for (int reflist = 0; reflist < 2; reflist++) {
    if (cu1->inter.mv_dir & (1 << reflist)) {
      if (cu1->inter.mv[reflist][0]  != cu2->inter.mv[reflist][0]  ||
          cu1->inter.mv[reflist][1]  != cu2->inter.mv[reflist][1]  ||
          cu1->inter.mv_ref[reflist] != cu2->inter.mv_ref[reflist]) {
        return false;
      }
    }
  }

  return true;
}

static bool add_merge_candidate(const cu_info_t *cand,
                                const cu_info_t *possible_duplicate1,
                                const cu_info_t *possible_duplicate2,
                                inter_merge_cand_t *merge_cand_out)
{
  if (!cand ||
      is_duplicate_candidate(cand, possible_duplicate1) ||
      is_duplicate_candidate(cand, possible_duplicate2)) {
    return false;
  }

  merge_cand_out->mv[0][0] = cand->inter.mv[0][0];
  merge_cand_out->mv[0][1] = cand->inter.mv[0][1];
  merge_cand_out->mv[1][0] = cand->inter.mv[1][0];
  merge_cand_out->mv[1][1] = cand->inter.mv[1][1];
  merge_cand_out->ref[0]   = cand->inter.mv_ref[0]; // L0/L1 references
  merge_cand_out->ref[1]   = cand->inter.mv_ref[1];
  merge_cand_out->dir      = cand->inter.mv_dir;
  return true;
}


static void hmvp_shift_lut(cu_info_t* lut, int32_t size, int32_t start, int32_t end) {

  if (end > MAX_NUM_HMVP_CANDS-1) end = MAX_NUM_HMVP_CANDS-1;
  if (end == 0 && size == 1) end = 1;
  for (int i = end-1; i >= start; i--) {
    memcpy(&lut[i + 1], &lut[i], sizeof(cu_info_t));
  }
}

static bool hmvp_push_lut_item(cu_info_t* lut, int32_t size, cu_info_t* cu) {

  int8_t duplicate = -1;

  for (int i = 0; i < size; i++) {
    if (is_duplicate_candidate(cu, &lut[i])) {
      duplicate = i;
      break;
    }
  }
  // If duplicate found, shift the whole lut up to the duplicate, otherwise to the end
  if(duplicate != 0) hmvp_shift_lut(lut, size, 0, duplicate == -1 ? MAX_NUM_HMVP_CANDS : duplicate);

  memcpy(lut, cu, sizeof(cu_info_t));

  return duplicate == -1;
}

/**
 * \brief Add processed CU motion vector to the HMVP look-up table
 *
 * \param state         encoder state
 * \param pic_x         block x position in pixels
 * \param pic_y         block y position in pixels
 * \param block_width   block width in pixels
 * \param block_height  block height in pixels
 * \param cu            current CU
 */
void kvz_hmvp_add_mv(const encoder_state_t* const state, uint32_t pic_x, uint32_t pic_y, uint32_t block_width, uint32_t block_height, cu_info_t* cu)
{
  //if (!cu.geoFlag && !cu.affine)
  if(cu->type == CU_INTER)
  {    

    const uint8_t parallel_merge_level = state->encoder_control->cfg.log2_parallel_merge_level;
    const uint32_t xBr = block_width + pic_x;
    const uint32_t yBr = block_height + pic_y;
    bool hmvp_possible = ((xBr >> parallel_merge_level) > (pic_x >> parallel_merge_level)) && ((yBr >> parallel_merge_level) > (pic_y >> parallel_merge_level));
    if (hmvp_possible) { // ToDo: check for IBC
      const uint32_t ctu_row = (pic_y >> LOG2_LCU_WIDTH);
      const uint32_t ctu_row_mul_five = ctu_row * MAX_NUM_HMVP_CANDS;

      
      bool add_row = hmvp_push_lut_item(&state->frame->hmvp_lut[ctu_row_mul_five], state->frame->hmvp_size[ctu_row], cu);
      if(add_row && state->frame->hmvp_size[ctu_row] < MAX_NUM_HMVP_CANDS) {
        state->frame->hmvp_size[ctu_row]++;
      }
    }
  }
}

static void round_avg_mv(mv_t* mvx, mv_t* mvy, int nShift)
{
  const int nOffset = 1 << (nShift - 1);
  *mvx = (*mvx + nOffset - (*mvx >= 0)) >> nShift;
  *mvy = (*mvy + nOffset - (*mvy >= 0)) >> nShift;
}

static bool different_mer(int32_t x, int32_t y, int32_t x2, int32_t y2, uint8_t parallel_merge_level) {

  if ((x >> parallel_merge_level) != (x2 >> parallel_merge_level) || (y >> parallel_merge_level) != (y2 >> parallel_merge_level))
  {
    return true;
  }

  return false;
}



void kvz_change_precision(int src, int dst, mv_t* hor, mv_t* ver) {

  const int shift = (int)dst - (int)src;
  if (shift >= 0)
  {
    *hor <<= shift;
    *ver <<= shift;
  }
  else
  {
    const int right_shift = -shift;
    const int offset = 1 << (right_shift - 1);
    *hor = *hor >= 0 ? (*hor + offset - 1) >> right_shift : (*hor + offset) >> right_shift;
    *ver = *ver >= 0 ? (*ver + offset - 1) >> right_shift : (*ver + offset) >> right_shift;
  }
}

void kvz_round_precision(int src, int dst, mv_t* hor, mv_t* ver) {
  kvz_change_precision(src, dst, hor, ver);
  kvz_change_precision(dst, src, hor, ver);
}

void kvz_round_precision_vector2d(int src, int dst, vector2d_t* mv) {
  mv_t hor = mv->x;
  mv_t ver = mv->y;
  kvz_change_precision(src, dst, &hor, &ver);
  kvz_change_precision(dst, src, &hor, &ver);
  mv->x = hor;
  mv->y = ver;
}

/**
 * \brief Get merge predictions for current block
 * \param state     the encoder state
 * \param x         block x position in SCU
 * \param y         block y position in SCU
 * \param width     block width
 * \param height    block height
 * \param use_a1    true, if candidate a1 can be used
 * \param use_b1    true, if candidate b1 can be used
 * \param mv_cand   Returns the merge candidates.
 * \param lcu       lcu containing the block
 * \return          number of merge candidates
 */
uint8_t kvz_inter_get_merge_cand(const encoder_state_t * const state,
                                 int32_t x, int32_t y,
                                 int32_t width, int32_t height,
                                 bool use_a1, bool use_b1,
                                 inter_merge_cand_t mv_cand[MRG_MAX_NUM_CANDS],
                                 lcu_t *lcu)
{
  uint8_t candidates = 0;
  int8_t zero_idx = 0;
  const uint8_t parallel_merge_level = state->encoder_control->cfg.log2_parallel_merge_level;
  merge_candidates_t merge_cand = { 0 };
  const uint8_t max_num_cands = state->encoder_control->cfg.max_merge;
  get_spatial_merge_candidates(x, y, width, height,
                               state->tile->frame->width,
                               state->tile->frame->height,
                               lcu,
                               &merge_cand, parallel_merge_level, state->encoder_control->cfg.wpp);

  const cu_info_t **a = merge_cand.a;
  const cu_info_t **b = merge_cand.b;
  const uint16_t mer[2] = {(x+width) >> parallel_merge_level, (y+height) >> parallel_merge_level };

  if (!use_a1) a[1] = NULL;
  if (!use_b1) b[1] = NULL;

  if (different_mer(x, y, x, y - 1, parallel_merge_level) && add_merge_candidate(b[1], NULL, NULL, &mv_cand[candidates])) candidates++;
  if (different_mer(x, y, x - 1, y, parallel_merge_level) && add_merge_candidate(a[1], b[1], NULL, &mv_cand[candidates])) candidates++;
  if (different_mer(x, y, x + 1, y - 1, parallel_merge_level) && add_merge_candidate(b[0], b[1], NULL, &mv_cand[candidates])) candidates++;
  if (different_mer(x, y, x - 1, y + 1, parallel_merge_level) && add_merge_candidate(a[0], a[1], NULL, &mv_cand[candidates])) candidates++;
  if (candidates < 4 &&
     different_mer(x, y, x - 1, y - 1, parallel_merge_level) && add_merge_candidate(b[2], a[1], b[1], &mv_cand[candidates])) candidates++;

  bool can_use_tmvp =
    state->encoder_control->cfg.tmvp_enable &&
    candidates < max_num_cands &&
    state->frame->ref->used_size;

  if (can_use_tmvp) {
    mv_cand[candidates].dir = 0;

    const int max_reflist = (state->frame->slicetype == KVZ_SLICE_B ? 1 : 0);
    for (int reflist = 0; reflist <= max_reflist; reflist++) {
      // Fetch temporal candidates for the current CU
      // ToDo: change collocated_from_l0_flag to allow L1 ref
      get_temporal_merge_candidates(state, x, y, width, height, 1, 0, &merge_cand);
      // TODO: enable L1 TMVP candidate
      // get_temporal_merge_candidates(state, x, y, width, height, 2, 0, &merge_cand);

      const cu_info_t *temporal_cand =
        (merge_cand.h != NULL) ? merge_cand.h : merge_cand.c3;

      if (add_temporal_candidate(state,
                                 // Reference index 0 is always used for
                                 // the temporal merge candidate.
                                 state->frame->ref_LX[0][0],
                                 temporal_cand,
                                 0,
                                 mv_cand[candidates].mv[reflist])) {
        mv_cand[candidates].ref[reflist] = 0;
        mv_cand[candidates].dir |= (1 << reflist);
      }
    }

    
    if (mv_cand[candidates].dir != 0) {
      bool add_tmvp = true;
      // Check for duplicate entries
      for (int i = 0; i < candidates; i++) {
        if (mv_cand[i].dir == mv_cand[candidates].dir &&
          (!(mv_cand[i].dir & 1) || (mv_cand[i].mv[0][0] == mv_cand[candidates].mv[0][0] &&
                                     mv_cand[i].mv[0][1] == mv_cand[candidates].mv[0][1])) &&
          (!(mv_cand[i].dir & 2) || (mv_cand[i].mv[1][0] == mv_cand[candidates].mv[1][0] &&
                                     mv_cand[i].mv[1][1] == mv_cand[candidates].mv[1][1]))) {
          add_tmvp = false;
          break;
        }
      }
      // No duplicates found, continue adding the candidate
      if(add_tmvp) candidates++;
    }
  }

  if (candidates == max_num_cands) return candidates;

  if (candidates != max_num_cands - 1) {
    const uint32_t ctu_row = (y >> LOG2_LCU_WIDTH);
    const uint32_t ctu_row_mul_five = ctu_row * MAX_NUM_HMVP_CANDS;
    int32_t num_cand = state->frame->hmvp_size[ctu_row];
    // ToDo: VVC: verify B-frames
    for (int i = 0; i < num_cand; i++) {
      const cu_info_t* hmvp_cand = &state->frame->hmvp_lut[ctu_row_mul_five + i];
      // ToDo: Add IBC condition
      if (i > 1 || ((!is_duplicate_candidate(hmvp_cand, a[1]))
                 && (!is_duplicate_candidate(hmvp_cand, b[1]))) ) {
        mv_cand[candidates].mv[0][0] = state->frame->hmvp_lut[ctu_row_mul_five + i].inter.mv[0][0];
        mv_cand[candidates].mv[0][1] = state->frame->hmvp_lut[ctu_row_mul_five + i].inter.mv[0][1];
        mv_cand[candidates].dir = state->frame->hmvp_lut[ctu_row_mul_five + i].inter.mv_dir;
        mv_cand[candidates].ref[0] = state->frame->hmvp_lut[ctu_row_mul_five + i].inter.mv_ref[0];
        if (state->frame->slicetype == KVZ_SLICE_B) {
          mv_cand[candidates].mv[1][0] = state->frame->hmvp_lut[ctu_row_mul_five + i].inter.mv[1][0];
          mv_cand[candidates].mv[1][1] = state->frame->hmvp_lut[ctu_row_mul_five + i].inter.mv[1][1];
          mv_cand[candidates].ref[1] = state->frame->hmvp_lut[ctu_row_mul_five + i].inter.mv_ref[1];
        }
        candidates++;
        if (candidates == max_num_cands - 1) break;
      }
    }
  }

  // pairwise-average candidates
  if (candidates > 1 && candidates < max_num_cands)
  {
    // calculate average MV for L0 and L1 seperately
    uint8_t inter_dir = 0;

    for (int reflist = 0; reflist < (state->frame->slicetype == KVZ_SLICE_B ? 2 : 1); reflist++)
    {
      const int16_t ref_i = mv_cand[0].dir & (reflist + 1) ? mv_cand[0].ref[reflist] : -1;
      const int16_t ref_j = mv_cand[1].dir & (reflist + 1) ? mv_cand[1].ref[reflist] : -1;

      // both MVs are invalid, skip
      if ((ref_i == -1) && (ref_j == -1))
      {
        continue;
      }

      inter_dir += 1 << reflist;
      // both MVs are valid, average these two MVs
      if ((ref_i != -1) && (ref_j != -1))
      {
        mv_t mv_i[2] = { mv_cand[0].mv[reflist][0], mv_cand[0].mv[reflist][1] };
        mv_t mv_j[2] = { mv_cand[1].mv[reflist][0], mv_cand[1].mv[reflist][1] };

        // average two MVs
        mv_t avg_mv[2] = { mv_i[0] + mv_j[0], mv_i[1] + mv_j[1] };
        
        round_avg_mv(&avg_mv[0], &avg_mv[1], 1);

        mv_cand[candidates].mv[reflist][0] = avg_mv[0];
        mv_cand[candidates].mv[reflist][1] = avg_mv[1];
        mv_cand[candidates].ref[reflist] = ref_i;
      }
      // only one MV is valid, take the only one MV
      else if (ref_i != -1)
      {
        int16_t mv_i[2] = { mv_cand[0].mv[reflist][0], mv_cand[0].mv[reflist][1] };

        mv_cand[candidates].mv[reflist][0] = mv_i[0];
        mv_cand[candidates].mv[reflist][1] = mv_i[1];
        mv_cand[candidates].ref[reflist] = ref_i;
      }
      else if (ref_j != -1)
      {
        int16_t mv_j[2] = { mv_cand[1].mv[reflist][0], mv_cand[1].mv[reflist][1] };

        mv_cand[candidates].mv[reflist][0] = mv_j[0];
        mv_cand[candidates].mv[reflist][1] = mv_j[1];
        mv_cand[candidates].ref[reflist] = ref_j;
      }
    }

    mv_cand[candidates].dir = inter_dir;
    if (inter_dir > 0)
    {
      candidates++;
    }
  }

  if (candidates == max_num_cands) return candidates;


  int num_ref = state->frame->ref->used_size;

  if (candidates < max_num_cands && state->frame->slicetype == KVZ_SLICE_B) {
    int j;
    int ref_negative = 0;
    int ref_positive = 0;
    for (j = 0; j < state->frame->ref->used_size; j++) {
      if (state->frame->ref->pocs[j] < state->frame->poc) {
        ref_negative++;
      } else {
        ref_positive++;
      }
    }
    num_ref = MIN(ref_negative, ref_positive);
  }

  // Add (0,0) prediction
  while (candidates != max_num_cands) {
    mv_cand[candidates].mv[0][0] = 0;
    mv_cand[candidates].mv[0][1] = 0;
    mv_cand[candidates].ref[0] = (zero_idx >= num_ref - 1) ? 0 : zero_idx;
    mv_cand[candidates].dir = 1;
    if (state->frame->slicetype == KVZ_SLICE_B) {
      mv_cand[candidates].ref[1] = mv_cand[candidates].ref[0];
      mv_cand[candidates].mv[1][0] = 0;
      mv_cand[candidates].mv[1][1] = 0;
      mv_cand[candidates].dir = 3;
    }
    zero_idx++;
    candidates++;
  }

  return candidates;
}
