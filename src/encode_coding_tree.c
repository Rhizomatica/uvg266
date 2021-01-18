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

#include "encode_coding_tree.h"

#include "cabac.h"
#include "context.h"
#include "cu.h"
#include "encoder.h"
#include "extras/crypto.h"
#include "imagelist.h"
#include "inter.h"
#include "intra.h"
#include "kvazaar.h"
#include "kvz_math.h"
#include "strategyselector.h"
#include "tables.h"
#include "videoframe.h"

static bool is_mts_allowed(encoder_state_t * const state, cu_info_t *const pred_cu)
{
  uint32_t ts_max_size = 1 << 2; //cu.cs->sps->getLog2MaxTransformSkipBlockSize();
  const int max_size = 32; // CU::isIntra(cu) ? MTS_INTRA_MAX_CU_SIZE : MTS_INTER_MAX_CU_SIZE;
  const int cu_width = LCU_WIDTH >> pred_cu->depth;
  const int cu_height = LCU_WIDTH >> pred_cu->depth;
  //bool mts_allowed = cu.chType == CHANNEL_TYPE_LUMA && compID == COMPONENT_Y;

  uint8_t mts_type = state->encoder_control->cfg.mts;
  bool mts_allowed = mts_type == KVZ_MTS_BOTH || (pred_cu->type == CU_INTRA ? mts_type == KVZ_MTS_INTRA : pred_cu->type == CU_INTER && mts_type == KVZ_MTS_INTER);
  mts_allowed &= cu_width <= max_size && cu_height <= max_size;
  //mts_allowed &= !cu.ispMode;
  //mts_allowed &= !cu.sbtInfo;
  mts_allowed &= !(pred_cu->bdpcmMode && cu_width <= ts_max_size && cu_height <= ts_max_size);
  return mts_allowed;
}

static void encode_mts_idx(encoder_state_t * const state,
  cabac_data_t * const cabac,
  cu_info_t *const pred_cu)
{
  //TransformUnit &tu = *cu.firstTU;
  int mts_idx = 5; // pred_cu->tr_idx;

  if (is_mts_allowed(state, pred_cu) && mts_idx != MTS_SKIP
       && !pred_cu->violates_mts_coeff_constraint
       && pred_cu->mts_last_scan_pos
       //&& cu.lfnstIdx == 0
    )
  {
    int symbol = mts_idx != MTS_DCT2_DCT2 ? 1 : 0;
    int ctx_idx = 0;

    cabac->cur_ctx = &(cabac->ctx.mts_idx_model[ctx_idx]);
    CABAC_BIN(cabac, symbol, "mts_idx");

    if (symbol)
    {
      ctx_idx = 1;
      for (int i = 0; i < 3; i++, ctx_idx++)
      {
        symbol = mts_idx > i + MTS_DST7_DST7 ? 1 : 0;
        cabac->cur_ctx = &(cabac->ctx.mts_idx_model[ctx_idx]);
        CABAC_BIN(cabac, symbol, "mts_idx");

        if (!symbol)
        {
          break;
        }
      }
    }
  }
}

/**
 * \brief Encode (X,Y) position of the last significant coefficient
 *
 * \param lastpos_x   X component of last coefficient
 * \param lastpos_y   Y component of last coefficient
 * \param width       Block width
 * \param height      Block height
 * \param type        plane type / luminance or chrominance
 * \param scan        scan type (diag, hor, ver) DEPRECATED?
 *
 * This method encodes the X and Y component within a block of the last
 * significant coefficient.
 */
void kvz_encode_last_significant_xy(cabac_data_t * const cabac,
                                       uint8_t lastpos_x, uint8_t lastpos_y,
                                       uint8_t width, uint8_t height,
                                       uint8_t type, uint8_t scan)
{
  const int index_x = kvz_math_floor_log2(width);
  const int index_y = kvz_math_floor_log2(width);
  const int prefix_ctx[8] = { 0, 0, 0, 3, 6, 10, 15, 21 };
  //ToDo: own ctx_offset and shift for X and Y 
  uint8_t ctx_offset_x = type ? 0 : prefix_ctx[index_x];
  uint8_t ctx_offset_y = type ? 0 : prefix_ctx[index_y];
  uint8_t shift_x = type ? CLIP(0, 2, width>>3) : (index_x+1)>>2;
  uint8_t shift_y = type ? CLIP(0, 2, width >> 3) : (index_y + 1) >> 2;

  cabac_ctx_t *base_ctx_x = (type ? cabac->ctx.cu_ctx_last_x_chroma : cabac->ctx.cu_ctx_last_x_luma);
  cabac_ctx_t *base_ctx_y = (type ? cabac->ctx.cu_ctx_last_y_chroma : cabac->ctx.cu_ctx_last_y_luma);

  const int group_idx_x = g_group_idx[lastpos_x];
  const int group_idx_y = g_group_idx[lastpos_y];

  // x prefix
  int last_x = 0;
  for (last_x = 0; last_x < group_idx_x; last_x++) {
    cabac->cur_ctx = &base_ctx_x[ctx_offset_x + (last_x >> shift_x)];
    CABAC_BIN(cabac, 1, "last_sig_coeff_x_prefix");
  }
  if (group_idx_x < ( /*width == 32 ? g_group_idx[15] : */g_group_idx[MIN(32, (int32_t)width) - 1])) {
    cabac->cur_ctx = &base_ctx_x[ctx_offset_x + (last_x >> shift_x)];
    CABAC_BIN(cabac, 0, "last_sig_coeff_x_prefix");
  }

  // y prefix
  int last_y = 0;
  for (last_y = 0; last_y < group_idx_y; last_y++) {
    cabac->cur_ctx = &base_ctx_y[ctx_offset_y + (last_y >> shift_y)];
    CABAC_BIN(cabac, 1, "last_sig_coeff_y_prefix");
  }
  if (group_idx_y < (/* height == 32 ? g_group_idx[15] : */g_group_idx[MIN(32, (int32_t)height) - 1])) {
    cabac->cur_ctx = &base_ctx_y[ctx_offset_y + (last_y >> shift_y)];
    CABAC_BIN(cabac, 0, "last_sig_coeff_y_prefix");
  }

  // last_sig_coeff_x_suffix
  if (group_idx_x > 3) {
    const int suffix = lastpos_x - g_min_in_group[group_idx_x];
    const int bits = (group_idx_x - 2) / 2;
    CABAC_BINS_EP(cabac, suffix, bits, "last_sig_coeff_x_suffix");
  }

  // last_sig_coeff_y_suffix
  if (group_idx_y > 3) {
    const int suffix = lastpos_y - g_min_in_group[group_idx_y];
    const int bits = (group_idx_y - 2) / 2;
    CABAC_BINS_EP(cabac, suffix, bits, "last_sig_coeff_y_suffix");
  }
}

static void encode_transform_unit(encoder_state_t * const state,
                                  int x, int y, int depth)
{
  assert(depth >= 1 && depth <= MAX_PU_DEPTH);

  const videoframe_t * const frame = state->tile->frame;
  const uint8_t width = LCU_WIDTH >> depth;
  const uint8_t width_c = (depth == MAX_PU_DEPTH ? width : width / 2);

  const cu_info_t *cur_pu = kvz_cu_array_at_const(frame->cu_array, x, y);

  int8_t scan_idx = kvz_get_scan_order(cur_pu->type, cur_pu->intra.mode, depth);

  if (state->encoder_control->chroma_format != KVZ_CSP_400) {
    // joint_cb_cr
    /*
    if (type == 2 && cbf_mask) {
      cabac->cur_ctx = &(cabac->ctx.joint_bc_br[0]);
      CABAC_BIN(cabac, 0, "joint_cb_cr");
    }
    */
  }

  int cbf_y = cbf_is_set(cur_pu->cbf, depth, COLOR_Y);

  if (cbf_y) {
    int x_local = x % LCU_WIDTH;
    int y_local = y % LCU_WIDTH;
    const coeff_t *coeff_y = &state->coeff->y[xy_to_zorder(LCU_WIDTH, x_local, y_local)];

    // CoeffNxN
    // Residual Coding

    kvz_encode_coeff_nxn(state,
                         &state->cabac,
                         coeff_y,
                         width,
                         0,
                         scan_idx,
                         cur_pu,
                         true);
  }

  if (depth == MAX_DEPTH + 1) {
    // For size 4x4 luma transform the corresponding chroma transforms are
    // also of size 4x4 covering 8x8 luma pixels. The residual is coded in
    // the last transform unit.
    if (x % 8 == 0 || y % 8 == 0) {
      // Not the last luma transform block so there is nothing more to do.
      return;
    } else {
      // Time to to code the chroma transform blocks. Move to the top-left
      // corner of the block.
      x -= 4;
      y -= 4;
      cur_pu = kvz_cu_array_at_const(frame->cu_array, x, y);
    }
  }

  bool chroma_cbf_set = cbf_is_set(cur_pu->cbf, depth, COLOR_U) ||
                        cbf_is_set(cur_pu->cbf, depth, COLOR_V);
  if (chroma_cbf_set) {
    int x_local = (x >> 1) % LCU_WIDTH_C;
    int y_local = (y >> 1) % LCU_WIDTH_C;
    scan_idx = kvz_get_scan_order(cur_pu->type, cur_pu->intra.mode_chroma, depth);

    const coeff_t *coeff_u = &state->coeff->u[xy_to_zorder(LCU_WIDTH_C, x_local, y_local)];
    const coeff_t *coeff_v = &state->coeff->v[xy_to_zorder(LCU_WIDTH_C, x_local, y_local)];

    if (cbf_is_set(cur_pu->cbf, depth, COLOR_U)) {
      kvz_encode_coeff_nxn(state, &state->cabac, coeff_u, width_c, 1, scan_idx, NULL, false);
    }

    if (cbf_is_set(cur_pu->cbf, depth, COLOR_V)) {
      kvz_encode_coeff_nxn(state, &state->cabac, coeff_v, width_c, 2, scan_idx, NULL, false);
    }
  }
}

/**
 * \param encoder
 * \param x_pu            Prediction units' x coordinate.
 * \param y_pu            Prediction units' y coordinate.
 * \param depth           Depth from LCU.
 * \param tr_depth        Depth from last CU.
 * \param parent_coeff_u  What was signaled at previous level for cbf_cb.
 * \param parent_coeff_v  What was signlaed at previous level for cbf_cr.
 */
static void encode_transform_coeff(encoder_state_t * const state,
                                   int32_t x,
                                   int32_t y,
                                   int8_t depth,
                                   int8_t tr_depth,
                                   uint8_t parent_coeff_u,
                                   uint8_t parent_coeff_v)
{
  cabac_data_t * const cabac = &state->cabac;
  //const encoder_control_t *const ctrl = state->encoder_control;
  const videoframe_t * const frame = state->tile->frame;

  const cu_info_t *cur_pu = kvz_cu_array_at_const(frame->cu_array, x, y);
  // Round coordinates down to a multiple of 8 to get the location of the
  // containing CU.
  const int x_cu = 8 * (x / 8);
  const int y_cu = 8 * (y / 8);
  const cu_info_t *cur_cu = kvz_cu_array_at_const(frame->cu_array, x_cu, y_cu);

  // NxN signifies implicit transform split at the first transform level.
  // There is a similar implicit split for inter, but it is only used when
  // transform hierarchy is not in use.
  //int intra_split_flag = (cur_cu->type == CU_INTRA && cur_cu->part_size == SIZE_NxN);

  // The implicit split by intra NxN is not counted towards max_tr_depth.
  /*
  int max_tr_depth;
  if (cur_cu->type == CU_INTRA) {
    max_tr_depth = ctrl->cfg.tr_depth_intra + intra_split_flag;
  } else {
    max_tr_depth = ctrl->tr_depth_inter;
  }
  */

  int8_t split = (LCU_WIDTH >> depth > TR_MAX_WIDTH);

 

  const int cb_flag_y = cbf_is_set(cur_pu->cbf, depth, COLOR_Y);
  const int cb_flag_u = cbf_is_set(cur_cu->cbf, depth, COLOR_U);
  const int cb_flag_v = cbf_is_set(cur_cu->cbf, depth, COLOR_V);

  // The split_transform_flag is not signaled when:
  // - transform size is greater than 32 (depth == 0)
  // - transform size is 4 (depth == MAX_PU_DEPTH)
  // - transform depth is max
  // - cu is intra NxN and it's the first split
  
  //ToDo: check BMS transform split in QTBT
  /*
  if (depth > 0 &&
      depth < MAX_PU_DEPTH &&
      tr_depth < max_tr_depth &&
      !(intra_split_flag && tr_depth == 0))
  {
    cabac->cur_ctx = &(cabac->ctx.trans_subdiv_model[5 - ((kvz_g_convert_to_bit[LCU_WIDTH] + 2) - depth)]);
    CABAC_BIN(cabac, split, "split_transform_flag");
  }
  */

  // Chroma cb flags are not signaled when one of the following:
  // - transform size is 4 (2x2 chroma transform doesn't exist)
  // - they have already been signaled to 0 previously
  // When they are not present they are inferred to be 0, except for size 4
  // when the flags from previous level are used.
  if (state->encoder_control->chroma_format != KVZ_CSP_400) {
    
    if (!split) {
      if (true) {
        assert(tr_depth < 5);
        cabac->cur_ctx = &(cabac->ctx.qt_cbf_model_cb[0]);
        CABAC_BIN(cabac, cb_flag_u, "cbf_cb");
      }
      if (true) {
        cabac->cur_ctx = &(cabac->ctx.qt_cbf_model_cr[cb_flag_u ? 1 : 0]);
        CABAC_BIN(cabac, cb_flag_v, "cbf_cr");
      }
    }
  }

  if (split) {
    uint8_t offset = LCU_WIDTH >> (depth + 1);
    int x2 = x + offset;
    int y2 = y + offset;
    encode_transform_coeff(state, x,  y,  depth + 1, tr_depth + 1, cb_flag_u, cb_flag_v);
    encode_transform_coeff(state, x2, y,  depth + 1, tr_depth + 1, cb_flag_u, cb_flag_v);
    encode_transform_coeff(state, x,  y2, depth + 1, tr_depth + 1, cb_flag_u, cb_flag_v);
    encode_transform_coeff(state, x2, y2, depth + 1, tr_depth + 1, cb_flag_u, cb_flag_v);
    return;
  }

  // Luma coded block flag is signaled when one of the following:
  // - prediction mode is intra
  // - transform depth > 0
  // - we have chroma coefficients at this level
  // When it is not present, it is inferred to be 1.
  if (cur_cu->type == CU_INTRA || tr_depth > 0 || cb_flag_u || cb_flag_v) {
      cabac->cur_ctx = &(cabac->ctx.qt_cbf_model_luma[0]);
      CABAC_BIN(cabac, cb_flag_y, "cbf_luma");
  }

  if (cb_flag_y | cb_flag_u | cb_flag_v) {
    if (state->must_code_qp_delta) {
      const int qp_pred      = kvz_get_cu_ref_qp(state, x_cu, y_cu, state->last_qp);
      const int qp_delta     = cur_cu->qp - qp_pred;
      assert(KVZ_BIT_DEPTH == 8 && "This range applies only to 8-bit encoding.");
      assert(qp_delta >= -26 && qp_delta <= 25 && "QP delta not in valid range [-26, 25]."); // This range applies only to 8-bit encoding
      const int qp_delta_abs = ABS(qp_delta);
      cabac_data_t* cabac    = &state->cabac;

      // cu_qp_delta_abs prefix
      cabac->cur_ctx = &cabac->ctx.cu_qp_delta_abs[0];
      kvz_cabac_write_unary_max_symbol(cabac, cabac->ctx.cu_qp_delta_abs, MIN(qp_delta_abs, 5), 1, 5);

      if (qp_delta_abs >= 5) {
        // cu_qp_delta_abs suffix
        kvz_cabac_write_ep_ex_golomb(state, cabac, qp_delta_abs - 5, 0);
      }

      if (qp_delta != 0) {
        CABAC_BIN_EP(cabac, (qp_delta >= 0 ? 0 : 1), "qp_delta_sign_flag");
      }

      state->must_code_qp_delta = false;
    }

    encode_transform_unit(state, x, y, depth);
  }
}

static void encode_inter_prediction_unit(encoder_state_t * const state,
                                         cabac_data_t * const cabac,
                                         const cu_info_t * const cur_cu,
                                         int x, int y, int width, int height,
                                         int depth)
{
  // Mergeflag
  int16_t num_cand = 0;
  cabac->cur_ctx = &(cabac->ctx.cu_merge_flag_ext_model);
  CABAC_BIN(cabac, cur_cu->merged, "MergeFlag");
  num_cand = MRG_MAX_NUM_CANDS;
  if (cur_cu->merged) { //merge
    if (num_cand > 1) {
      int32_t ui;
      for (ui = 0; ui < num_cand - 1; ui++) {
        int32_t symbol = (ui != cur_cu->merge_idx);
        if (ui == 0) {
          cabac->cur_ctx = &(cabac->ctx.cu_merge_idx_ext_model);
          CABAC_BIN(cabac, symbol, "MergeIndex");
        } else {
          CABAC_BIN_EP(cabac,symbol,"MergeIndex");
        }
        if (symbol == 0) break;
      }
    }
  } else {
    if (state->frame->slicetype == KVZ_SLICE_B) {
      // Code Inter Dir
      uint8_t inter_dir = cur_cu->inter.mv_dir-1;

      if (cur_cu->part_size == SIZE_2Nx2N || (LCU_WIDTH >> depth) != 8) {
        // ToDo: large CTU changes this inter_dir context selection
        cabac->cur_ctx = &(cabac->ctx.inter_dir[depth]);
        CABAC_BIN(cabac, (inter_dir == 2), "inter_pred_idc");
      }
      if (inter_dir < 2) {
        cabac->cur_ctx = &(cabac->ctx.inter_dir[4]);
        CABAC_BIN(cabac, inter_dir, "inter_pred_idc");
      }
    }

    for (uint32_t ref_list_idx = 0; ref_list_idx < 2; ref_list_idx++) {
      if (!(cur_cu->inter.mv_dir & (1 << ref_list_idx))) {
        continue;
      }

      // size of the current reference index list (L0/L1)
      uint8_t ref_LX_size = state->frame->ref_LX_size[ref_list_idx];

      if (ref_LX_size > 1) {
        // parseRefFrmIdx
        int32_t ref_frame = cur_cu->inter.mv_ref[ref_list_idx];

        cabac->cur_ctx = &(cabac->ctx.cu_ref_pic_model[0]);
        CABAC_BIN(cabac, (ref_frame != 0), "ref_idx_lX");

        if (ref_frame > 0) {
          ref_frame--;

          int32_t ref_num = ref_LX_size - 2;

          for (int32_t i = 0; i < ref_num; ++i) {
            const uint32_t symbol = (i == ref_frame) ? 0 : 1;

            if (i == 0) {
              cabac->cur_ctx = &cabac->ctx.cu_ref_pic_model[1];
              CABAC_BIN(cabac, symbol, "ref_idx_lX");
            } else {
              CABAC_BIN_EP(cabac, symbol, "ref_idx_lX");
            }
            if (symbol == 0) break;
          }
        }
      }

      if (state->frame->ref_list != REF_PIC_LIST_1 || cur_cu->inter.mv_dir != 3) {

        int16_t mv_cand[2][2];
        kvz_inter_get_mv_cand_cua(
            state,
            x, y, width, height,
            mv_cand, cur_cu, ref_list_idx);

        uint8_t cu_mv_cand = CU_GET_MV_CAND(cur_cu, ref_list_idx);
        const int32_t mvd_hor = cur_cu->inter.mv[ref_list_idx][0] - mv_cand[cu_mv_cand][0];
        const int32_t mvd_ver = cur_cu->inter.mv[ref_list_idx][1] - mv_cand[cu_mv_cand][1];

        kvz_encode_mvd(state, cabac, mvd_hor, mvd_ver);
      }

      // Signal which candidate MV to use
      kvz_cabac_write_unary_max_symbol(cabac,
                                       &cabac->ctx.mvp_idx_model,
                                       CU_GET_MV_CAND(cur_cu, ref_list_idx),
                                       1,
                                       AMVP_MAX_NUM_CANDS - 1);

    } // for ref_list
  } // if !merge
}

static void encode_intra_coding_unit(encoder_state_t * const state,
                                     cabac_data_t * const cabac,
                                     const cu_info_t * const cur_cu,
                                     int x, int y, int depth)
{
  const videoframe_t * const frame = state->tile->frame;
  uint8_t intra_pred_mode_actual[4];
  uint8_t *intra_pred_mode = intra_pred_mode_actual;

  uint8_t intra_pred_mode_chroma = cur_cu->intra.mode_chroma;
  int8_t intra_preds[4][INTRA_MPM_COUNT] = {{-1, -1, -1, -1, -1, -1},{-1, -1, -1, -1, -1, -1},{-1, -1, -1, -1, -1, -1},{-1, -1, -1, -1, -1, -1}};
  int8_t mpm_preds[4] = {-1, -1, -1, -1};
  uint32_t flag[4];

  /*
  if ((cur_cu->type == CU_INTRA && (LCU_WIDTH >> cur_cu->depth <= 32))) {
    cabac->cur_ctx = &(cabac->ctx.bdpcm_mode[0]);
    CABAC_BIN(cabac, cur_cu->bdpcmMode > 0 ? 1 : 0, "bdpcm_mode");
    if (cur_cu->bdpcmMode) {
      cabac->cur_ctx = &(cabac->ctx.bdpcm_mode[1]);
      CABAC_BIN(cabac, cur_cu->bdpcmMode > 1 ? 1 : 0, "bdpcm_mode > 1");
    }
  }
  */

  #if ENABLE_PCM == 1
  // Code must start after variable initialization
  kvz_cabac_encode_bin_trm(cabac, 0); // IPCMFlag == 0
  #endif

  /*
  if (cur_cu->type == 1 && (LCU_WIDTH >> depth <= 32)) {
    cabac->cur_ctx = &(cabac->ctx.bdpcm_mode[0]);
    CABAC_BIN(cabac, 0, "bdpcm_mode");
  }
  */

  const int num_pred_units = kvz_part_mode_num_parts[cur_cu->part_size];

  //ToDo: update multi_ref_lines variable when it's something else than constant 3
  int multi_ref_lines = 3;
  /*
  if(isp_enable_flag){ //ToDo: implement flag value to be something else than constant zero
    for (int i = 0; i < num_pred_units; i++) {
      if (multi_ref_lines > 1) {
        cabac->cur_ctx = &(cabac->ctx.multi_ref_line[0]);
        CABAC_BIN(cabac, cur_cu->intra.multi_ref_idx != 0, "multi_ref_line_0");
        if (multi_ref_lines > 2 && cur_cu->intra.multi_ref_idx != 0) {
          cabac->cur_ctx = &(cabac->ctx.multi_ref_line[1]);
          CABAC_BIN(cabac, cur_cu->intra.multi_ref_idx != 1, "multi_ref_line_1");
          if (multi_ref_lines > 3 && cur_cu->intra.multi_ref_idx != 1) {
            cabac->cur_ctx = &(cabac->ctx.multi_ref_line[2]);
            CABAC_BIN(cabac, cur_cu->intra.multi_ref_idx != 3, "multi_ref_line_2");
          }
        }
      }
    }
  }
  */

  // Intra Subpartition mode
  uint32_t width = (LCU_WIDTH >> depth);
  uint32_t height = (LCU_WIDTH >> depth);

  bool enough_samples = kvz_g_convert_to_bit[width] + kvz_g_convert_to_bit[height] > (kvz_g_convert_to_bit[4 /* MIN_TB_SIZEY*/] << 1);
  uint8_t isp_mode = 0;
  // ToDo: add height comparison
  //isp_mode += ((width > TR_MAX_WIDTH) || !enough_samples) ? 1 : 0;
  //isp_mode += ((height > TR_MAX_WIDTH) || !enough_samples) ? 2 : 0;
  bool allow_isp = enough_samples;

  if (0 && cur_cu->type == 1/*intra*/ && (y % LCU_WIDTH) != 0) {
    cabac->cur_ctx = &(cabac->ctx.multi_ref_line[0]);
    CABAC_BIN(cabac, 0, "multi_ref_line");
  }


  // ToDo: update real usage, these if clauses as such don't make any sense
  if (isp_mode != 0) {
    if (isp_mode) {
      cabac->cur_ctx = &(cabac->ctx.intra_subpart_model[0]);
      CABAC_BIN(cabac, 0, "intra_subPartitions");
    } else {
      cabac->cur_ctx = &(cabac->ctx.intra_subpart_model[0]);
      CABAC_BIN(cabac, 1, "intra_subPartitions");
      // ToDo: complete this if-clause
      if (isp_mode == 3) {
        cabac->cur_ctx = &(cabac->ctx.intra_subpart_model[1]);
        CABAC_BIN(cabac, allow_isp - 1, "intra_subPart_ver_hor");
      }
    }
  }

  // PREDINFO CODING
  // If intra prediction mode is found from the predictors,
  // it can be signaled with two EP's. Otherwise we can send
  // 5 EP bins with the full predmode
  // ToDo: fix comments for VVC
  const int cu_width = LCU_WIDTH >> depth;

  cabac->cur_ctx = &(cabac->ctx.intra_luma_mpm_flag_model);
  for (int j = 0; j < num_pred_units; ++j) {
    const int pu_x = PU_GET_X(cur_cu->part_size, cu_width, x, j);
    const int pu_y = PU_GET_Y(cur_cu->part_size, cu_width, y, j);
    const cu_info_t *cur_pu = kvz_cu_array_at_const(frame->cu_array, pu_x, pu_y);

    const cu_info_t *left_pu = NULL;
    const cu_info_t *above_pu = NULL;

    if (pu_x > 0) {
      assert(pu_x >> 2 > 0);
      left_pu = kvz_cu_array_at_const(frame->cu_array, pu_x - 1, pu_y + cu_width - 1);
    }
    // Don't take the above PU across the LCU boundary.
    if (pu_y % LCU_WIDTH > 0 && pu_y > 0) {
      assert(pu_y >> 2 > 0);
      above_pu = kvz_cu_array_at_const(frame->cu_array, pu_x + cu_width - 1, pu_y - 1);
    }


    kvz_intra_get_dir_luma_predictor(pu_x, pu_y,
                                      intra_preds[j],
                                      cur_pu,
                                      left_pu, above_pu);


    intra_pred_mode_actual[j] = cur_pu->intra.mode;

    for (int i = 0; i < INTRA_MPM_COUNT; i++) {
      if (intra_preds[j][i] == intra_pred_mode[j]) {
        mpm_preds[j] = (int8_t)i;
        break;
      }
    }
    // Is the mode in the MPM array or not
    flag[j] = (mpm_preds[j] == -1) ? 0 : 1;
    if (true||!(cur_pu->intra.multi_ref_idx || (isp_mode))) {
      CABAC_BIN(cabac, flag[j], "prev_intra_luma_pred_flag");
    }
  }

  


  for (int j = 0; j < num_pred_units; ++j) {
    // Signal index of the prediction mode in the prediction list, if it is there
    if (flag[j]) {
      
      const int pu_x = PU_GET_X(cur_cu->part_size, cu_width, x, j);
      const int pu_y = PU_GET_Y(cur_cu->part_size, cu_width, y, j);
      const cu_info_t *cur_pu = kvz_cu_array_at_const(frame->cu_array, pu_x, pu_y);
      cabac->cur_ctx = &(cabac->ctx.luma_planar_model[(isp_mode ? 0 : 1)]);
      if (true||cur_pu->intra.multi_ref_idx == 0) {
        CABAC_BIN(cabac, (mpm_preds[j] > 0 ? 1 : 0), "mpm_idx_luma_planar");
      }
      //CABAC_BIN_EP(cabac, (mpm_preds[j] > 0 ? 1 : 0), "mpm_idx");
      if (mpm_preds[j] > 0) {
        CABAC_BIN_EP(cabac, (mpm_preds[j] > 1 ? 1 : 0), "mpm_idx");
      }
      if (mpm_preds[j] > 1) {
        CABAC_BIN_EP(cabac, (mpm_preds[j] > 2 ? 1 : 0), "mpm_idx");
      }
      if (mpm_preds[j] > 2) {
        CABAC_BIN_EP(cabac, (mpm_preds[j] > 3 ? 1 : 0), "mpm_idx");
      }
      if (mpm_preds[j] > 3) {
        CABAC_BIN_EP(cabac, (mpm_preds[j] > 4 ? 1 : 0), "mpm_idx");
      }
    } else {
      // Signal the actual prediction mode.
      int32_t tmp_pred = intra_pred_mode[j];

      uint8_t intra_preds_temp[INTRA_MPM_COUNT+2];
      memcpy(intra_preds_temp, intra_preds[j], sizeof(int8_t)*3);
      memcpy(intra_preds_temp+4, &intra_preds[j][3], sizeof(int8_t)*3);
      intra_preds_temp[3] = 255;
      intra_preds_temp[7] = 255;

      // Improvised merge sort
      // Sort prediction list from lowest to highest.
      if (intra_preds_temp[0] > intra_preds_temp[1]) SWAP(intra_preds_temp[0], intra_preds_temp[1], uint8_t);
      if (intra_preds_temp[0] > intra_preds_temp[2]) SWAP(intra_preds_temp[0], intra_preds_temp[2], uint8_t);
      if (intra_preds_temp[1] > intra_preds_temp[2]) SWAP(intra_preds_temp[1], intra_preds_temp[2], uint8_t);

      if (intra_preds_temp[4] > intra_preds_temp[5]) SWAP(intra_preds_temp[4], intra_preds_temp[5], uint8_t);
      if (intra_preds_temp[4] > intra_preds_temp[6]) SWAP(intra_preds_temp[4], intra_preds_temp[6], uint8_t);
      if (intra_preds_temp[5] > intra_preds_temp[6]) SWAP(intra_preds_temp[5], intra_preds_temp[6], uint8_t);

      // Merge two subarrays
      int32_t array1 = 0;
      int32_t array2 = 4;
      for (int item = 0; item < INTRA_MPM_COUNT; item++) {
        if (intra_preds_temp[array1] < intra_preds_temp[array2]) {
          intra_preds[j][item] = intra_preds_temp[array1];
          array1++;
        } else {
          intra_preds[j][item] = intra_preds_temp[array2];
          array2++;
        }
      }

      // Reduce the index of the signaled prediction mode according to the
      // prediction list, as it has been already signaled that it's not one
      // of the prediction modes.
      for (int i = INTRA_MPM_COUNT-1; i >= 0; i--) {
        if (tmp_pred > intra_preds[j][i]) {
          tmp_pred--;
        }
      }
      
      kvz_cabac_encode_trunc_bin(cabac, tmp_pred, 67 - INTRA_MPM_COUNT);
    }
  }

  // Code chroma prediction mode.
  if (state->encoder_control->chroma_format != KVZ_CSP_400) {
    unsigned pred_mode = 0;
    unsigned chroma_pred_modes[8] = {0, 50, 18, 1, 67, 68, 69, 70};
    const int pu_x = PU_GET_X(cur_cu->part_size, cu_width, x, 0);
    const int pu_y = PU_GET_Y(cur_cu->part_size, cu_width, y, 0);
    const cu_info_t *first_pu = kvz_cu_array_at_const(frame->cu_array, pu_x, pu_y);
    int8_t chroma_intra_dir = first_pu->intra.mode_chroma;
    int8_t luma_intra_dir = first_pu->intra.mode;

    bool derived_mode = 1;// chroma_intra_dir == 70;
    cabac->cur_ctx = &(cabac->ctx.chroma_pred_model);
    CABAC_BIN(cabac, derived_mode ? 0 : 1, "intra_chroma_pred_mode");


    if (false/* !derived_mode*/) {
      /*for (int i = 0; i < 4; i++) {
        if (luma_intra_dir == chroma_pred_modes[i]) {
          chroma_pred_modes[i] = 66;
          break;
        }
      }*/
      for (; pred_mode < 8; pred_mode++) {
        if (chroma_intra_dir == chroma_pred_modes[pred_mode]) {
          break;
        }
      }
      /*else if (intra_pred_mode_chroma == 66) {
        // Angular 66 mode is possible only if intra pred mode is one of the
        // possible chroma pred modes, in which case it is signaled with that
        // duplicate mode.
        for (int i = 0; i < 4; ++i) {
          if (intra_pred_mode_actual[0] == chroma_pred_modes[i]) pred_mode = i;
        }
      }
      else {
        for (int i = 0; i < 4; ++i) {
          if (intra_pred_mode_chroma == chroma_pred_modes[i]) pred_mode = i;
        }
      }

      // pred_mode == 67 mean intra_pred_mode_chroma is something that can't
      // be coded.
      assert(pred_mode != 67);
      */
      /**
       * Table 9-35 - Binarization for intra_chroma_pred_mode
       *   intra_chroma_pred_mode  bin_string
       *                        4           0
       *                        0         100
       *                        1         101
       *                        2         110
       *                        3         111
       * Table 9-37 - Assignment of ctxInc to syntax elements with context coded bins
       *   intra_chroma_pred_mode[][] = 0, bypass, bypass
       */
      /*cabac->cur_ctx = &(cabac->ctx.chroma_pred_model[0]);
      if (pred_mode == 68) {
        CABAC_BIN(cabac, 0, "intra_chroma_pred_mode");
      }
      else {
        CABAC_BIN(cabac, 1, "intra_chroma_pred_mode");*/
        CABAC_BINS_EP(cabac, 0/*pred_mode*/, 2, "intra_chroma_pred_mode");
      //}
    }
  }

  encode_transform_coeff(state, x, y, depth, 0, 0, 0);

  encode_mts_idx(state, cabac, cur_cu);
}

/**
static void encode_part_mode(encoder_state_t * const state,
                             cabac_data_t * const cabac,
                             const cu_info_t * const cur_cu,
                             int depth)
{
  // Binarization from Table 9-34 of the HEVC spec:
  //
  //                |   log2CbSize >     |    log2CbSize ==
  //                |   MinCbLog2SizeY   |    MinCbLog2SizeY
  // -------+-------+----------+---------+-----------+----------
  //  pred  | part  | AMP      | AMP     |           |
  //  mode  | mode  | disabled | enabled | size == 8 | size > 8
  // -------+-------+----------+---------+-----------+----------
  //  intra | 2Nx2N |        -         - |         1          1
  //        |   NxN |        -         - |         0          0
  // -------+-------+--------------------+----------------------
  //  inter | 2Nx2N |        1         1 |         1          1
  //        |  2NxN |       01       011 |        01         01
  //        |  Nx2N |       00       001 |        00        001
  //        |   NxN |        -         - |         -        000
  //        | 2NxnU |        -      0100 |         -          -
  //        | 2NxnD |        -      0101 |         -          -
  //        | nLx2N |        -      0000 |         -          -
  //        | nRx2N |        -      0001 |         -          -
  // -------+-------+--------------------+----------------------
  //
  //
  // Context indices from Table 9-37 of the HEVC spec:
  //
  //                                      binIdx
  //                               |  0  1  2       3
  // ------------------------------+------------------
  //  log2CbSize == MinCbLog2SizeY |  0  1  2  bypass
  //  log2CbSize >  MinCbLog2SizeY |  0  1  3  bypass
  // ------------------------------+------------------

  if (cur_cu->type == CU_INTRA) {
    if (depth == MAX_DEPTH) {
      cabac->cur_ctx = &(cabac->ctx.part_size_model[0]);
      if (cur_cu->part_size == SIZE_2Nx2N) {
        CABAC_BIN(cabac, 1, "part_mode 2Nx2N");
      } else {
        CABAC_BIN(cabac, 0, "part_mode NxN");
      }
    }
  } else {

    cabac->cur_ctx = &(cabac->ctx.part_size_model[0]);
    if (cur_cu->part_size == SIZE_2Nx2N) {
      CABAC_BIN(cabac, 1, "part_mode 2Nx2N");
      return;
    }
    CABAC_BIN(cabac, 0, "part_mode split");

    cabac->cur_ctx = &(cabac->ctx.part_size_model[1]);
    if (cur_cu->part_size == SIZE_2NxN ||
        cur_cu->part_size == SIZE_2NxnU ||
        cur_cu->part_size == SIZE_2NxnD) {
      CABAC_BIN(cabac, 1, "part_mode vertical");
    } else {
      CABAC_BIN(cabac, 0, "part_mode horizontal");
    }

    if (state->encoder_control->cfg.amp_enable && depth < MAX_DEPTH) {
      cabac->cur_ctx = &(cabac->ctx.part_size_model[3]);

      if (cur_cu->part_size == SIZE_2NxN ||
          cur_cu->part_size == SIZE_Nx2N) {
        CABAC_BIN(cabac, 1, "part_mode SMP");
        return;
      }
      CABAC_BIN(cabac, 0, "part_mode AMP");

      if (cur_cu->part_size == SIZE_2NxnU ||
          cur_cu->part_size == SIZE_nLx2N) {
        CABAC_BINS_EP(cabac, 0, 1, "part_mode AMP");
      } else {
        CABAC_BINS_EP(cabac, 1, 1, "part_mode AMP");
      }
    }
  }
}
**/

void kvz_encode_coding_tree(encoder_state_t * const state,
                            uint16_t x,
                            uint16_t y,
                            uint8_t depth)
{
  cabac_data_t * const cabac = &state->cabac;
  const encoder_control_t * const ctrl = state->encoder_control;
  const videoframe_t * const frame = state->tile->frame;
  cu_info_t *cur_cu   = kvz_cu_array_at_const(frame->cu_array, x, y);

  const int cu_width = LCU_WIDTH >> depth;
  const int half_cu  = cu_width >> 1;

  const cu_info_t *left_cu  = NULL;
  if (x > 0) {
    left_cu = kvz_cu_array_at_const(frame->cu_array, x - 1, y);
  }
  const cu_info_t *above_cu = NULL;
  if (y > 0) {
    above_cu = kvz_cu_array_at_const(frame->cu_array, x, y - 1);
  }

  uint8_t split_flag = GET_SPLITDATA(cur_cu, depth);
  uint8_t split_model = 0;

  // Absolute coordinates
  uint16_t abs_x = x + state->tile->offset_x;
  uint16_t abs_y = y + state->tile->offset_y;

  // Check for slice border
  bool border_x = ctrl->in.width  < abs_x + cu_width;
  bool border_y = ctrl->in.height < abs_y + cu_width;
  bool border_split_x = ctrl->in.width  >= abs_x + (LCU_WIDTH >> MAX_DEPTH) + half_cu;
  bool border_split_y = ctrl->in.height >= abs_y + (LCU_WIDTH >> MAX_DEPTH) + half_cu;
  bool border = border_x || border_y; /*!< are we in any border CU */

  if (depth <= ctrl->max_qp_delta_depth) {
    state->must_code_qp_delta = true;
  }

  // When not in MAX_DEPTH, insert split flag and split the blocks if needed
  if (depth != MAX_DEPTH) {

    // Implisit split flag when on border
    // Exception made in VVC with flag not being implicit if the BT can be used for
    // horizontal or vertical split, then this flag tells if QT or BT is used

    bool no_split, allow_qt, bh_split, bv_split, th_split, tv_split;
    no_split = allow_qt = bh_split = bv_split = th_split = tv_split = true;
    if(depth > MAX_DEPTH) allow_qt = false;
    // ToDo: update this when btt is actually used
    bool allow_btt = false;// when mt_depth < MAX_BT_DEPTH

    

    uint8_t implicit_split_mode = KVZ_NO_SPLIT;
    //bool implicit_split = border;
    bool bottom_left_available = (abs_x >= 0) && ((abs_y + cu_width - 1) < ctrl->in.height);
    bool top_right_available = ((abs_x + cu_width - 1) < ctrl->in.width) && (abs_y >= 0);

    /*
    if((depth >= 1 && (border_x != border_y))) implicit_split = false;
    if (state->frame->slicetype != KVZ_SLICE_I) {
      if (border_x != border_y) implicit_split = false;
      if (!bottom_left_available && top_right_available) implicit_split = false;
      if (!top_right_available && bottom_left_available) implicit_split = false;
    }
    */


    if (!bottom_left_available && !top_right_available && allow_qt) {
      implicit_split_mode = KVZ_QUAD_SPLIT;
    } else if (!bottom_left_available && allow_btt) {
      implicit_split_mode = KVZ_HORZ_SPLIT;
    } else if (!top_right_available && allow_btt) {
      implicit_split_mode = KVZ_VERT_SPLIT;
    } else if (!bottom_left_available || !top_right_available) {
      implicit_split_mode = KVZ_QUAD_SPLIT;
    }

    //split_flag = implicit_split_mode != KVZ_NO_SPLIT;

    // Check split conditions
    if (implicit_split_mode != KVZ_NO_SPLIT) {
      no_split = th_split = tv_split = false;
      bh_split = (implicit_split_mode == KVZ_HORZ_SPLIT);
      bv_split = (implicit_split_mode == KVZ_VERT_SPLIT);
    }

    if (!allow_btt) {
      bh_split = bv_split = th_split = tv_split = false;
    }

    bool allow_split = allow_qt | bh_split | bv_split | th_split | tv_split;

    split_flag |= implicit_split_mode != KVZ_NO_SPLIT;

    if (no_split && allow_split) {
      split_model = 0;
      
      // Get left and top block split_flags and if they are present and true, increase model number
      // ToDo: should use height and width to increase model, PU_GET_W() ?
      if (left_cu && PU_GET_H(left_cu->part_size,LCU_WIDTH>>left_cu->depth,0) < LCU_WIDTH>>depth) {
        split_model++;
      }

      if (above_cu && PU_GET_W(above_cu->part_size, LCU_WIDTH >> above_cu->depth, 0) < LCU_WIDTH >> depth) {
        split_model++;
      }

      uint32_t split_num = 0;
      if (allow_qt) split_num+=2;
      if (bh_split) split_num++;
      if (bv_split) split_num++;
      if (th_split) split_num++;
      if (tv_split) split_num++;

      if (split_num > 0) split_num--;

      split_model += 3 * (split_num >> 1);

      cabac->cur_ctx = &(cabac->ctx.split_flag_model[split_model]);
      CABAC_BIN(cabac, split_flag, "SplitFlag");
      //fprintf(stdout, "split_model=%d  %d / %d / %d / %d / %d\n", split_model, allow_qt, bh_split, bv_split, th_split, tv_split);
    }

    bool qt_split = split_flag || implicit_split_mode == KVZ_QUAD_SPLIT;

    if (!(implicit_split_mode == KVZ_NO_SPLIT) && (allow_qt && allow_btt)) {
      split_model = (left_cu && GET_SPLITDATA(left_cu, depth)) + (above_cu && GET_SPLITDATA(above_cu, depth)) + (depth < 2 ? 0 : 3);
      cabac->cur_ctx = &(cabac->ctx.qt_split_flag_model[split_model]);
      CABAC_BIN(cabac, qt_split, "QT_SplitFlag");
    }

    // Only signal split when it is not implicit, currently only Qt split supported
    if (!(implicit_split_mode == KVZ_NO_SPLIT) && !qt_split && (bh_split | bv_split | th_split | tv_split)) {

      split_model = 0;

      // Get left and top block split_flags and if they are present and true, increase model number
      if (left_cu && GET_SPLITDATA(left_cu, depth) == 1) {
        split_model++;
      }

      if (above_cu && GET_SPLITDATA(above_cu, depth) == 1) {
        split_model++;
      }
      split_model += (depth > 2 ? 0 : 3);

      cabac->cur_ctx = &(cabac->ctx.qt_split_flag_model[split_model]);
      CABAC_BIN(cabac, split_flag, "split_cu_mode");
    }

    if (split_flag || border) {
      // Split blocks and remember to change x and y block positions
      kvz_encode_coding_tree(state, x, y, depth + 1);

      if (!border_x || border_split_x) {
        kvz_encode_coding_tree(state, x + half_cu, y, depth + 1);
      }
      if (!border_y || border_split_y) {
        kvz_encode_coding_tree(state, x, y + half_cu, depth + 1);
      }
      if (!border || (border_split_x && border_split_y)) {
        kvz_encode_coding_tree(state, x + half_cu, y + half_cu, depth + 1);
      }
      return;
    }
  }

  //ToDo: check if we can actually split
  //ToDo: Implement MT split
  if (depth < MAX_PU_DEPTH)
  {
   // cabac->cur_ctx = &(cabac->ctx.trans_subdiv_model[5 - ((kvz_g_convert_to_bit[LCU_WIDTH] + 2) - depth)]);
   // CABAC_BIN(cabac, 0, "split_transform_flag");
  }

  if (ctrl->cfg.lossless) {
    cabac->cur_ctx = &cabac->ctx.cu_transquant_bypass;
    CABAC_BIN(cabac, 1, "cu_transquant_bypass_flag");
  }

  // Encode skip flag
  if (state->frame->slicetype != KVZ_SLICE_I) {

    int8_t ctx_skip = 0;

    if (left_cu && left_cu->skipped) {
      ctx_skip++;
    }
    if (above_cu && above_cu->skipped) {
      ctx_skip++;
    }

    cabac->cur_ctx = &(cabac->ctx.cu_skip_flag_model[ctx_skip]);
    CABAC_BIN(cabac, cur_cu->skipped, "SkipFlag");

    if (cur_cu->skipped) {
      int16_t num_cand = MRG_MAX_NUM_CANDS;
      if (num_cand > 1) {
        for (int ui = 0; ui < num_cand - 1; ui++) {
          int32_t symbol = (ui != cur_cu->merge_idx);
          if (ui == 0) {
            cabac->cur_ctx = &(cabac->ctx.cu_merge_idx_ext_model);
            CABAC_BIN(cabac, symbol, "MergeIndex");
          } else {
            CABAC_BIN_EP(cabac,symbol,"MergeIndex");
          }
          if (symbol == 0) {
            break;
          }
        }
      }
      goto end;
    }
  }

  // Prediction mode
  if (state->frame->slicetype != KVZ_SLICE_I) {

    int8_t ctx_predmode = 0;

    if ((left_cu && left_cu->type == CU_INTRA) || (above_cu && above_cu->type == CU_INTRA)) {
      ctx_predmode=1;
    }

    cabac->cur_ctx = &(cabac->ctx.cu_pred_mode_model[ctx_predmode]);
    CABAC_BIN(cabac, (cur_cu->type == CU_INTRA), "PredMode");
  }

  // part_mode
  //encode_part_mode(state, cabac, cur_cu, depth);

  

#if ENABLE_PCM
  // Code IPCM block
  if (FORCE_PCM || cur_cu->type == CU_PCM) {
    kvz_cabac_encode_bin_trm(cabac, 1); // IPCMFlag == 1
    kvz_cabac_finish(cabac);
    kvz_bitstream_add_rbsp_trailing_bits(cabac->stream);
    
    // PCM sample
    kvz_pixel *base_y = &frame->source->y[x + y * ctrl->in.width];
    kvz_pixel *base_u = &frame->source->u[x / 2 + y / 2 * ctrl->in.width / 2];
    kvz_pixel *base_v = &frame->source->v[x / 2 + y / 2 * ctrl->in.width / 2];

    kvz_pixel *rec_base_y = &frame->rec->y[x + y * ctrl->in.width];
    kvz_pixel *rec_base_u = &frame->rec->u[x / 2 + y / 2 * ctrl->in.width / 2];
    kvz_pixel *rec_base_v = &frame->rec->v[x / 2 + y / 2 * ctrl->in.width / 2];

    // Luma
    for (unsigned y_px = 0; y_px < LCU_WIDTH >> depth; y_px++) {
      for (unsigned x_px = 0; x_px < LCU_WIDTH >> depth; x_px++) {
        kvz_bitstream_put(cabac->stream, base_y[x_px + y_px * ctrl->in.width], 8);
        rec_base_y[x_px + y_px * ctrl->in.width] = base_y[x_px + y_px * ctrl->in.width];
      }
    }

    // Chroma
    if (ctrl->chroma_format != KVZ_CSP_400) {
      for (unsigned y_px = 0; y_px < LCU_WIDTH >> (depth + 1); y_px++) {
        for (unsigned x_px = 0; x_px < LCU_WIDTH >> (depth + 1); x_px++) {
          kvz_bitstream_put(cabac->stream, base_u[x_px + y_px * (ctrl->in.width >> 1)], 8);
          rec_base_u[x_px + y_px * (ctrl->in.width >> 1)] = base_u[x_px + y_px * (ctrl->in.width >> 1)];
        }
      }
      for (unsigned y_px = 0; y_px < LCU_WIDTH >> (depth + 1); y_px++) {
        for (unsigned x_px = 0; x_px < LCU_WIDTH >> (depth + 1); x_px++) {
          kvz_bitstream_put(cabac->stream, base_v[x_px + y_px * (ctrl->in.width >> 1)], 8);
          rec_base_v[x_px + y_px * (ctrl->in.width >> 1)] = base_v[x_px + y_px * (ctrl->in.width >> 1)];
        }
      }
    }
    kvz_cabac_start(cabac);
  } else 
#endif

  if (cur_cu->type == CU_INTER) {
    const int num_pu = kvz_part_mode_num_parts[cur_cu->part_size];

    for (int i = 0; i < num_pu; ++i) {
      const int pu_x = PU_GET_X(cur_cu->part_size, cu_width, x, i);
      const int pu_y = PU_GET_Y(cur_cu->part_size, cu_width, y, i);
      const int pu_w = PU_GET_W(cur_cu->part_size, cu_width, i);
      const int pu_h = PU_GET_H(cur_cu->part_size, cu_width, i);
      const cu_info_t *cur_pu = kvz_cu_array_at_const(frame->cu_array, pu_x, pu_y);

      encode_inter_prediction_unit(state, cabac, cur_pu, pu_x, pu_y, pu_w, pu_h, depth);
    }

    {
      int cbf = cbf_is_set_any(cur_cu->cbf, depth);
      // Only need to signal coded block flag if not skipped or merged
      // skip = no coded residual, merge = coded residual
      if (cur_cu->part_size != SIZE_2Nx2N || !cur_cu->merged) {
        cabac->cur_ctx = &(cabac->ctx.cu_qt_root_cbf_model);
        CABAC_BIN(cabac, cbf, "rqt_root_cbf");
      }
      // Code (possible) coeffs to bitstream

      if (cbf) {
        encode_transform_coeff(state, x, y, depth, 0, 0, 0);
      }
    }
  } else if (cur_cu->type == CU_INTRA) {
    cur_cu->mts_last_scan_pos = false;
    cur_cu->violates_mts_coeff_constraint = false;
    encode_intra_coding_unit(state, cabac, cur_cu, x, y, depth);
  }

  else {
    // CU type not set. Should not happen.
    assert(0);
    exit(1);
  }

end:

  if (is_last_cu_in_qg(state, x, y, depth)) {
    state->last_qp = cur_cu->qp;
  }

}


void kvz_encode_mvd(encoder_state_t * const state,
                    cabac_data_t *cabac,
                    int32_t mvd_hor,
                    int32_t mvd_ver)
{
  const int8_t hor_abs_gr0 = mvd_hor != 0;
  const int8_t ver_abs_gr0 = mvd_ver != 0;
  const uint32_t mvd_hor_abs = abs(mvd_hor);
  const uint32_t mvd_ver_abs = abs(mvd_ver);

  cabac->cur_ctx = &cabac->ctx.cu_mvd_model[0];
  CABAC_BIN(cabac, (mvd_hor != 0), "abs_mvd_greater0_flag_hor");
  CABAC_BIN(cabac, (mvd_ver != 0), "abs_mvd_greater0_flag_ver");

  cabac->cur_ctx = &cabac->ctx.cu_mvd_model[1];
  if (hor_abs_gr0) {
    CABAC_BIN(cabac, (mvd_hor_abs>1), "abs_mvd_greater1_flag_hor");
  }
  if (ver_abs_gr0) {
    CABAC_BIN(cabac, (mvd_ver_abs>1), "abs_mvd_greater1_flag_ver");
  }

  if (hor_abs_gr0) {
    if (mvd_hor_abs > 1) {
      kvz_cabac_write_ep_ex_golomb(state, cabac, mvd_hor_abs - 2, 1);
    }
    uint32_t mvd_hor_sign = (mvd_hor > 0) ? 0 : 1;
    CABAC_BIN_EP(cabac, mvd_hor_sign, "mvd_sign_flag_hor");
  }
  if (ver_abs_gr0) {
    if (mvd_ver_abs > 1) {
      kvz_cabac_write_ep_ex_golomb(state, cabac, mvd_ver_abs - 2, 1);
    }
    uint32_t mvd_ver_sign = mvd_ver > 0 ? 0 : 1;
    CABAC_BIN_EP(cabac, mvd_ver_sign, "mvd_sign_flag_ver");
  }
}
