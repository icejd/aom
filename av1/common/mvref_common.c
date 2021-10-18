/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <stdlib.h>

#include "av1/common/mv.h"
#include "av1/common/mvref_common.h"
#include "av1/common/pred_common.h"
#include "av1/common/warped_motion.h"

#define MFMV_STACK_SIZE 3

// Although we assign 32 bit integers, all the values are strictly under 14
// bits.
static int div_mult[32] = { 0,    16384, 8192, 5461, 4096, 3276, 2730, 2340,
                            2048, 1820,  1638, 1489, 1365, 1260, 1170, 1092,
                            1024, 963,   910,  862,  819,  780,  744,  712,
                            682,  655,   630,  606,  585,  564,  546,  528 };

// TODO(jingning): Consider the use of lookup table for (num / den)
// altogether.
static AOM_INLINE void get_mv_projection(MV *output, MV ref, int num, int den) {
  den = AOMMIN(den, MAX_FRAME_DISTANCE);
  num = num > 0 ? AOMMIN(num, MAX_FRAME_DISTANCE)
                : AOMMAX(num, -MAX_FRAME_DISTANCE);
  const int mv_row =
      ROUND_POWER_OF_TWO_SIGNED(ref.row * num * div_mult[den], 14);
  const int mv_col =
      ROUND_POWER_OF_TWO_SIGNED(ref.col * num * div_mult[den], 14);
  const int clamp_max = MV_UPP - 1;
  const int clamp_min = MV_LOW + 1;
  output->row = (int16_t)clamp(mv_row, clamp_min, clamp_max);
  output->col = (int16_t)clamp(mv_col, clamp_min, clamp_max);
}

void av1_copy_frame_mvs(const AV1_COMMON *const cm,
                        const MB_MODE_INFO *const mi, int mi_row, int mi_col,
                        int x_mis, int y_mis) {
  const int frame_mvs_stride = ROUND_POWER_OF_TWO(cm->mi_params.mi_cols, 1);
  MV_REF *frame_mvs =
      cm->cur_frame->mvs + (mi_row >> 1) * frame_mvs_stride + (mi_col >> 1);
  x_mis = ROUND_POWER_OF_TWO(x_mis, 1);
  y_mis = ROUND_POWER_OF_TWO(y_mis, 1);
  int w, h;

  for (h = 0; h < y_mis; h++) {
    MV_REF *mv = frame_mvs;
    for (w = 0; w < x_mis; w++) {
#if CONFIG_NEW_REF_SIGNALING
      mv->ref_frame = INVALID_IDX;
#else
      mv->ref_frame = NONE_FRAME;
#endif  // CONFIG_NEW_REF_SIGNALING
      mv->mv.as_int = 0;

      for (int idx = 0; idx < 2; ++idx) {
#if CONFIG_NEW_REF_SIGNALING
        MV_REFERENCE_FRAME_NRS ref_frame = mi->ref_frame_nrs[idx];
        if (ref_frame != INTRA_FRAME_NRS && ref_frame != INVALID_IDX)
#else
        MV_REFERENCE_FRAME ref_frame = mi->ref_frame[idx];
        if (ref_frame > INTRA_FRAME)
#endif  // CONFIG_NEW_REF_SIGNALING
        {
#if CONFIG_NEW_REF_SIGNALING
          int8_t ref_idx = cm->ref_frame_side_nrs[ref_frame];
#else
          int8_t ref_idx = cm->ref_frame_side[ref_frame];
#endif  // CONFIG_NEW_REF_SIGNALING
          if (ref_idx) continue;
          if ((abs(mi->mv[idx].as_mv.row) > REFMVS_LIMIT) ||
              (abs(mi->mv[idx].as_mv.col) > REFMVS_LIMIT))
            continue;
          mv->ref_frame = ref_frame;
          mv->mv.as_int = mi->mv[idx].as_int;
        }
      }
      mv++;
    }
    frame_mvs += frame_mvs_stride;
  }
}

#if CONFIG_NEW_REF_SIGNALING
static AOM_INLINE void add_ref_mv_candidate(
    const MB_MODE_INFO *const candidate, const MV_REFERENCE_FRAME_NRS rf[2],
    uint8_t *refmv_count, uint8_t *ref_match_count, uint8_t *newmv_count,
    CANDIDATE_MV *ref_mv_stack, uint16_t *ref_mv_weight,
    int_mv *gm_mv_candidates, const WarpedMotionParams *gm_params,
    uint16_t weight) {
  if (!is_inter_block(candidate)) return;
  assert(weight % 2 == 0);
  int index, ref;

  if (rf[1] == INVALID_IDX) {
    // single reference frame
    for (ref = 0; ref < 2; ++ref) {
      if (candidate->ref_frame_nrs[ref] == rf[0]) {
        const int is_gm_block =
            is_global_mv_block(candidate, gm_params[rf[0]].wmtype);
        const int_mv this_refmv =
            is_gm_block ? gm_mv_candidates[0] : get_block_mv(candidate, ref);
        for (index = 0; index < *refmv_count; ++index) {
          if (ref_mv_stack[index].this_mv.as_int == this_refmv.as_int) {
            ref_mv_weight[index] += weight;
            break;
          }
        }

        // Add a new item to the list.
        if (index == *refmv_count && *refmv_count < MAX_REF_MV_STACK_SIZE) {
          ref_mv_stack[index].this_mv = this_refmv;
          ref_mv_weight[index] = weight;
          ++(*refmv_count);
        }
        if (have_newmv_in_inter_mode(candidate->mode)) ++*newmv_count;
        ++*ref_match_count;
      }
    }
  } else {
    // compound reference frame
    if (candidate->ref_frame_nrs[0] == rf[0] &&
        candidate->ref_frame_nrs[1] == rf[1]) {
      int_mv this_refmv[2];

      for (ref = 0; ref < 2; ++ref) {
        if (is_global_mv_block(candidate, gm_params[rf[ref]].wmtype))
          this_refmv[ref] = gm_mv_candidates[ref];
        else
          this_refmv[ref] = get_block_mv(candidate, ref);
      }

      for (index = 0; index < *refmv_count; ++index) {
        if ((ref_mv_stack[index].this_mv.as_int == this_refmv[0].as_int) &&
            (ref_mv_stack[index].comp_mv.as_int == this_refmv[1].as_int)) {
          ref_mv_weight[index] += weight;
          break;
        }
      }

      // Add a new item to the list.
      if (index == *refmv_count && *refmv_count < MAX_REF_MV_STACK_SIZE) {
        ref_mv_stack[index].this_mv = this_refmv[0];
        ref_mv_stack[index].comp_mv = this_refmv[1];
        ref_mv_weight[index] = weight;
        ++(*refmv_count);
      }
      if (have_newmv_in_inter_mode(candidate->mode)) ++*newmv_count;
      ++*ref_match_count;
    }
  }
}
#else
static AOM_INLINE void add_ref_mv_candidate(
    const MB_MODE_INFO *const candidate, const MV_REFERENCE_FRAME rf[2],
    uint8_t *refmv_count, uint8_t *ref_match_count, uint8_t *newmv_count,
    CANDIDATE_MV *ref_mv_stack, uint16_t *ref_mv_weight,
    int_mv *gm_mv_candidates, const WarpedMotionParams *gm_params,
    uint16_t weight) {
  if (!is_inter_block(candidate)) return;
  assert(weight % 2 == 0);
  int index, ref;

  if (rf[1] == NONE_FRAME) {
    // single reference frame
    for (ref = 0; ref < 2; ++ref) {
      if (candidate->ref_frame[ref] == rf[0]) {
        const int is_gm_block =
            is_global_mv_block(candidate, gm_params[rf[0]].wmtype);
        const int_mv this_refmv =
            is_gm_block ? gm_mv_candidates[0] : get_block_mv(candidate, ref);
        for (index = 0; index < *refmv_count; ++index) {
          if (ref_mv_stack[index].this_mv.as_int == this_refmv.as_int) {
            ref_mv_weight[index] += weight;
            break;
          }
        }

        // Add a new item to the list.
        if (index == *refmv_count && *refmv_count < MAX_REF_MV_STACK_SIZE) {
          ref_mv_stack[index].this_mv = this_refmv;
          ref_mv_weight[index] = weight;
          ++(*refmv_count);
        }
        if (have_newmv_in_inter_mode(candidate->mode)) ++*newmv_count;
        ++*ref_match_count;
      }
    }
  } else {
    // compound reference frame
    if (candidate->ref_frame[0] == rf[0] && candidate->ref_frame[1] == rf[1]) {
      int_mv this_refmv[2];

      for (ref = 0; ref < 2; ++ref) {
        if (is_global_mv_block(candidate, gm_params[rf[ref]].wmtype))
          this_refmv[ref] = gm_mv_candidates[ref];
        else
          this_refmv[ref] = get_block_mv(candidate, ref);
      }

      for (index = 0; index < *refmv_count; ++index) {
        if ((ref_mv_stack[index].this_mv.as_int == this_refmv[0].as_int) &&
            (ref_mv_stack[index].comp_mv.as_int == this_refmv[1].as_int)) {
          ref_mv_weight[index] += weight;
          break;
        }
      }

      // Add a new item to the list.
      if (index == *refmv_count && *refmv_count < MAX_REF_MV_STACK_SIZE) {
        ref_mv_stack[index].this_mv = this_refmv[0];
        ref_mv_stack[index].comp_mv = this_refmv[1];
        ref_mv_weight[index] = weight;
        ++(*refmv_count);
      }
      if (have_newmv_in_inter_mode(candidate->mode)) ++*newmv_count;
      ++*ref_match_count;
    }
  }
}
#endif  // CONFIG_NEW_REF_SIGNALING

static AOM_INLINE void scan_row_mbmi(
    const AV1_COMMON *cm, const MACROBLOCKD *xd,
#if CONFIG_EXT_RECUR_PARTITIONS
    int mi_row,
#endif  // CONFIG_EXT_RECUR_PARTITIONS
    int mi_col,
#if CONFIG_NEW_REF_SIGNALING
    const MV_REFERENCE_FRAME_NRS rf[2],
#else
    const MV_REFERENCE_FRAME rf[2],
#endif  // CONFIG_NEW_REF_SIGNALING
    int row_offset, CANDIDATE_MV *ref_mv_stack, uint16_t *ref_mv_weight,
    uint8_t *refmv_count, uint8_t *ref_match_count, uint8_t *newmv_count,
    int_mv *gm_mv_candidates, int max_row_offset, int *processed_rows) {
  int end_mi = AOMMIN(xd->width, cm->mi_params.mi_cols - mi_col);
  end_mi = AOMMIN(end_mi, mi_size_wide[BLOCK_64X64]);
  const int width_8x8 = mi_size_wide[BLOCK_8X8];
  const int width_16x16 = mi_size_wide[BLOCK_16X16];
  int col_offset = 0;
  // TODO(jingning): Revisit this part after cb4x4 is stable.
  if (abs(row_offset) > 1) {
    col_offset = 1;
    if ((mi_col & 0x01) && xd->width < width_8x8) --col_offset;
  }
  const int use_step_16 = (xd->width >= 16);
  MB_MODE_INFO **const candidate_mi0 = xd->mi + row_offset * xd->mi_stride;

  for (int i = 0; i < end_mi;) {
#if CONFIG_EXT_RECUR_PARTITIONS
    const int sb_mi_size = mi_size_wide[cm->seq_params.sb_size];
    const int mask_row = mi_row & (sb_mi_size - 1);
    const int mask_col = mi_col & (sb_mi_size - 1);
    const int ref_mask_row = mask_row + row_offset;
    const int ref_mask_col = mask_col + col_offset + i;
    if (ref_mask_row >= 0) {
      if (ref_mask_col >= sb_mi_size) break;

      const int ref_offset =
          ref_mask_row * xd->is_mi_coded_stride + ref_mask_col;
      if (!xd->is_mi_coded[ref_offset]) break;
    }
#endif  // CONFIG_EXT_RECUR_PARTITIONS
    const MB_MODE_INFO *const candidate = candidate_mi0[col_offset + i];
    const int candidate_bsize = candidate->sb_type;
    const int n4_w = mi_size_wide[candidate_bsize];
    int len = AOMMIN(xd->width, n4_w);
    if (use_step_16)
      len = AOMMAX(width_16x16, len);
    else if (abs(row_offset) > 1)
      len = AOMMAX(len, width_8x8);

    uint16_t weight = 2;
    if (xd->width >= width_8x8 && xd->width <= n4_w) {
      uint16_t inc = AOMMIN(-max_row_offset + row_offset + 1,
                            mi_size_high[candidate_bsize]);
      // Obtain range used in weight calculation.
      weight = AOMMAX(weight, inc);
      // Update processed rows.
      *processed_rows = inc - row_offset - 1;
    }

#if CONFIG_NEW_REF_SIGNALING
    add_ref_mv_candidate(candidate, rf, refmv_count, ref_match_count,
                         newmv_count, ref_mv_stack, ref_mv_weight,
                         gm_mv_candidates, cm->global_motion_nrs, len * weight);
#else
    add_ref_mv_candidate(candidate, rf, refmv_count, ref_match_count,
                         newmv_count, ref_mv_stack, ref_mv_weight,
                         gm_mv_candidates, cm->global_motion, len * weight);
#endif  // CONFIG_NEW_REF_SIGNALING

    i += len;
  }
}

static AOM_INLINE void scan_col_mbmi(
    const AV1_COMMON *cm, const MACROBLOCKD *xd, int mi_row,
#if CONFIG_EXT_RECUR_PARTITIONS
    int mi_col,
#endif  // CONFIG_EXT_RECUR_PARTITIONS
#if CONFIG_NEW_REF_SIGNALING
    const MV_REFERENCE_FRAME_NRS rf[2],
#else
    const MV_REFERENCE_FRAME rf[2],
#endif  // CONFIG_NEW_REF_SIGNALING
    int col_offset, CANDIDATE_MV *ref_mv_stack, uint16_t *ref_mv_weight,
    uint8_t *refmv_count, uint8_t *ref_match_count, uint8_t *newmv_count,
    int_mv *gm_mv_candidates, int max_col_offset, int *processed_cols) {
  int end_mi = AOMMIN(xd->height, cm->mi_params.mi_rows - mi_row);
  end_mi = AOMMIN(end_mi, mi_size_high[BLOCK_64X64]);
  const int n8_h_8 = mi_size_high[BLOCK_8X8];
  const int n8_h_16 = mi_size_high[BLOCK_16X16];
  int i;
  int row_offset = 0;
  if (abs(col_offset) > 1) {
    row_offset = 1;
    if ((mi_row & 0x01) && xd->height < n8_h_8) --row_offset;
  }
  const int use_step_16 = (xd->height >= 16);

  for (i = 0; i < end_mi;) {
#if CONFIG_EXT_RECUR_PARTITIONS
    const int sb_mi_size = mi_size_wide[cm->seq_params.sb_size];
    const int mask_row = mi_row & (sb_mi_size - 1);
    const int mask_col = mi_col & (sb_mi_size - 1);
    const int ref_mask_row = mask_row + row_offset + i;
    const int ref_mask_col = mask_col + col_offset;
    if (ref_mask_col >= 0) {
      if (ref_mask_row >= sb_mi_size) break;
      const int ref_offset =
          ref_mask_row * xd->is_mi_coded_stride + ref_mask_col;
      if (!xd->is_mi_coded[ref_offset]) break;
    }
#endif  // CONFIG_EXT_RECUR_PARTITIONS
    const MB_MODE_INFO *const candidate =
        xd->mi[(row_offset + i) * xd->mi_stride + col_offset];
    const int candidate_bsize = candidate->sb_type;
    const int n4_h = mi_size_high[candidate_bsize];
    int len = AOMMIN(xd->height, n4_h);
    if (use_step_16)
      len = AOMMAX(n8_h_16, len);
    else if (abs(col_offset) > 1)
      len = AOMMAX(len, n8_h_8);

    int weight = 2;
    if (xd->height >= n8_h_8 && xd->height <= n4_h) {
      int inc = AOMMIN(-max_col_offset + col_offset + 1,
                       mi_size_wide[candidate_bsize]);
      // Obtain range used in weight calculation.
      weight = AOMMAX(weight, inc);
      // Update processed cols.
      *processed_cols = inc - col_offset - 1;
    }

#if CONFIG_NEW_REF_SIGNALING
    add_ref_mv_candidate(candidate, rf, refmv_count, ref_match_count,
                         newmv_count, ref_mv_stack, ref_mv_weight,
                         gm_mv_candidates, cm->global_motion_nrs, len * weight);
#else
    add_ref_mv_candidate(candidate, rf, refmv_count, ref_match_count,
                         newmv_count, ref_mv_stack, ref_mv_weight,
                         gm_mv_candidates, cm->global_motion, len * weight);
#endif  // CONFIG_NEW_REF_SIGNALING

    i += len;
  }
}

static AOM_INLINE void scan_blk_mbmi(
    const AV1_COMMON *cm, const MACROBLOCKD *xd, const int mi_row,
    const int mi_col,
#if CONFIG_NEW_REF_SIGNALING
    const MV_REFERENCE_FRAME_NRS rf[2],
#else
    const MV_REFERENCE_FRAME rf[2],
#endif  // CONFIG_NEW_REF_SIGNALING
    int row_offset, int col_offset, CANDIDATE_MV *ref_mv_stack,
    uint16_t *ref_mv_weight, uint8_t *ref_match_count, uint8_t *newmv_count,
    int_mv *gm_mv_candidates, uint8_t *refmv_count) {
  const TileInfo *const tile = &xd->tile;
  POSITION mi_pos;

  mi_pos.row = row_offset;
  mi_pos.col = col_offset;

  if (is_inside(tile, mi_col, mi_row, &mi_pos)) {
    const MB_MODE_INFO *const candidate =
        xd->mi[mi_pos.row * xd->mi_stride + mi_pos.col];
    const int len = mi_size_wide[BLOCK_8X8];

#if CONFIG_NEW_REF_SIGNALING
    add_ref_mv_candidate(candidate, rf, refmv_count, ref_match_count,
                         newmv_count, ref_mv_stack, ref_mv_weight,
                         gm_mv_candidates, cm->global_motion_nrs, 2 * len);
#else
    add_ref_mv_candidate(candidate, rf, refmv_count, ref_match_count,
                         newmv_count, ref_mv_stack, ref_mv_weight,
                         gm_mv_candidates, cm->global_motion, 2 * len);
#endif  // CONFIG_NEW_REF_SIGNALING
  }     // Analyze a single 8x8 block motion information.
}

static int has_top_right(const AV1_COMMON *cm, const MACROBLOCKD *xd,
                         int mi_row, int mi_col, int n4_w) {
  const int sb_mi_size = mi_size_wide[cm->seq_params.sb_size];
  const int mask_row = mi_row & (sb_mi_size - 1);
  const int mask_col = mi_col & (sb_mi_size - 1);

  if (n4_w > mi_size_wide[BLOCK_64X64]) return 0;

  const int tr_mask_row = mask_row - 1;
  const int tr_mask_col = mask_col + n4_w;
  int has_tr;

  if (tr_mask_row < 0) {
    // The top-right block is in a superblock above the current sb row. If it is
    // in the current tile or a previously coded one, it has been coded.
    // Otherwise later the tile boundary checker will figure out whether it is
    // available.
    has_tr = 1;
  } else if (tr_mask_col >= sb_mi_size) {
    // The top-right block is in the superblock on the right side, therefore it
    // is not coded yet.
    has_tr = 0;
  } else {
    // For a general case, we use is_mi_coded array for the current superblock
    // to figure out the availability.
    const int tr_offset = tr_mask_row * xd->is_mi_coded_stride + tr_mask_col;

    has_tr = xd->is_mi_coded[tr_offset];
  }

  return has_tr;
}

static int check_sb_border(const int mi_row, const int mi_col,
                           const int row_offset, const int col_offset) {
  const int sb_mi_size = mi_size_wide[BLOCK_64X64];
  const int row = mi_row & (sb_mi_size - 1);
  const int col = mi_col & (sb_mi_size - 1);

  if (row + row_offset < 0 || row + row_offset >= sb_mi_size ||
      col + col_offset < 0 || col + col_offset >= sb_mi_size)
    return 0;

  return 1;
}

static int add_tpl_ref_mv(const AV1_COMMON *cm, const MACROBLOCKD *xd,
                          int mi_row, int mi_col,
#if CONFIG_NEW_REF_SIGNALING
                          MV_REFERENCE_FRAME_NRS ref_frame,
#else
                          MV_REFERENCE_FRAME ref_frame,
#endif  // CONFIG_NEW_REF_SIGNALING
                          int blk_row, int blk_col, int_mv *gm_mv_candidates,
                          uint8_t *const refmv_count,
                          CANDIDATE_MV ref_mv_stack[MAX_REF_MV_STACK_SIZE],
                          uint16_t ref_mv_weight[MAX_REF_MV_STACK_SIZE],
                          int16_t *mode_context) {
  POSITION mi_pos;
  mi_pos.row = (mi_row & 0x01) ? blk_row : blk_row + 1;
  mi_pos.col = (mi_col & 0x01) ? blk_col : blk_col + 1;

  if (!is_inside(&xd->tile, mi_col, mi_row, &mi_pos)) return 0;

  const TPL_MV_REF *prev_frame_mvs =
      cm->tpl_mvs +
      ((mi_row + mi_pos.row) >> 1) * (cm->mi_params.mi_stride >> 1) +
      ((mi_col + mi_pos.col) >> 1);
  if (prev_frame_mvs->mfmv0.as_int == INVALID_MV) return 0;

#if CONFIG_NEW_REF_SIGNALING
  MV_REFERENCE_FRAME_NRS rf[2];
  av1_set_ref_frame_nrs(rf, ref_frame);
#else
  MV_REFERENCE_FRAME rf[2];
  av1_set_ref_frame(rf, ref_frame);
#endif  // CONFIG_NEW_REF_SIGNALING

  const uint16_t weight_unit = 1;  // mi_size_wide[BLOCK_8X8];
  const int cur_frame_index = cm->cur_frame->order_hint;
#if CONFIG_NEW_REF_SIGNALING
  const RefCntBuffer *const buf_0 = get_ref_frame_buf_nrs(cm, rf[0]);
#else
  const RefCntBuffer *const buf_0 = get_ref_frame_buf(cm, rf[0]);
#endif  // CONFIG_NEW_REF_SIGNALING
  const int frame0_index = buf_0->order_hint;
  const int cur_offset_0 = get_relative_dist(&cm->seq_params.order_hint_info,
                                             cur_frame_index, frame0_index);
  int idx;
  const MvSubpelPrecision fr_mv_precision = cm->features.fr_mv_precision;

  int_mv this_refmv;
  get_mv_projection(&this_refmv.as_mv, prev_frame_mvs->mfmv0.as_mv,
                    cur_offset_0, prev_frame_mvs->ref_frame_offset);
  lower_mv_precision(&this_refmv.as_mv, fr_mv_precision);

#if CONFIG_NEW_REF_SIGNALING
  if (rf[1] == INVALID_IDX)
#else
  if (rf[1] == NONE_FRAME)
#endif  // CONFIG_NEW_REF_SIGNALING
  {
    if (blk_row == 0 && blk_col == 0) {
      if (abs(this_refmv.as_mv.row - gm_mv_candidates[0].as_mv.row) >= 16 ||
          abs(this_refmv.as_mv.col - gm_mv_candidates[0].as_mv.col) >= 16)
        mode_context[ref_frame] |= (1 << GLOBALMV_OFFSET);
    }

    for (idx = 0; idx < *refmv_count; ++idx)
      if (this_refmv.as_int == ref_mv_stack[idx].this_mv.as_int) break;

    if (idx < *refmv_count) ref_mv_weight[idx] += 2 * weight_unit;

    if (idx == *refmv_count && *refmv_count < MAX_REF_MV_STACK_SIZE) {
      ref_mv_stack[idx].this_mv.as_int = this_refmv.as_int;
      ref_mv_weight[idx] = 2 * weight_unit;
      ++(*refmv_count);
    }
  } else {
    // Process compound inter mode
#if CONFIG_NEW_REF_SIGNALING
    const RefCntBuffer *const buf_1 = get_ref_frame_buf_nrs(cm, rf[1]);
#else
    const RefCntBuffer *const buf_1 = get_ref_frame_buf(cm, rf[1]);
#endif  // CONFIG_NEW_REF_SIGNALING
    const int frame1_index = buf_1->order_hint;
    const int cur_offset_1 = get_relative_dist(&cm->seq_params.order_hint_info,
                                               cur_frame_index, frame1_index);
    int_mv comp_refmv;
    get_mv_projection(&comp_refmv.as_mv, prev_frame_mvs->mfmv0.as_mv,
                      cur_offset_1, prev_frame_mvs->ref_frame_offset);
    lower_mv_precision(&comp_refmv.as_mv, fr_mv_precision);

    if (blk_row == 0 && blk_col == 0) {
      if (abs(this_refmv.as_mv.row - gm_mv_candidates[0].as_mv.row) >= 16 ||
          abs(this_refmv.as_mv.col - gm_mv_candidates[0].as_mv.col) >= 16 ||
          abs(comp_refmv.as_mv.row - gm_mv_candidates[1].as_mv.row) >= 16 ||
          abs(comp_refmv.as_mv.col - gm_mv_candidates[1].as_mv.col) >= 16)
        mode_context[ref_frame] |= (1 << GLOBALMV_OFFSET);
    }

    for (idx = 0; idx < *refmv_count; ++idx) {
      if (this_refmv.as_int == ref_mv_stack[idx].this_mv.as_int &&
          comp_refmv.as_int == ref_mv_stack[idx].comp_mv.as_int)
        break;
    }

    if (idx < *refmv_count) ref_mv_weight[idx] += 2 * weight_unit;

    if (idx == *refmv_count && *refmv_count < MAX_REF_MV_STACK_SIZE) {
      ref_mv_stack[idx].this_mv.as_int = this_refmv.as_int;
      ref_mv_stack[idx].comp_mv.as_int = comp_refmv.as_int;
      ref_mv_weight[idx] = 2 * weight_unit;
      ++(*refmv_count);
    }
  }

  return 1;
}

static AOM_INLINE void process_compound_ref_mv_candidate(
    const MB_MODE_INFO *const candidate, const AV1_COMMON *const cm,
#if CONFIG_NEW_REF_SIGNALING
    const MV_REFERENCE_FRAME_NRS *const rf,
#else
    const MV_REFERENCE_FRAME *const rf,
#endif  // CONFIG_NEW_REF_SIGNALING
    int_mv ref_id[2][2], int ref_id_count[2], int_mv ref_diff[2][2],
    int ref_diff_count[2]) {
  for (int rf_idx = 0; rf_idx < 2; ++rf_idx) {
#if CONFIG_NEW_REF_SIGNALING
    MV_REFERENCE_FRAME_NRS can_rf = candidate->ref_frame_nrs[rf_idx];
#else
    MV_REFERENCE_FRAME can_rf = candidate->ref_frame[rf_idx];
#endif  // CONFIG_NEW_REF_SIGNALING

    for (int cmp_idx = 0; cmp_idx < 2; ++cmp_idx) {
      if (can_rf == rf[cmp_idx] && ref_id_count[cmp_idx] < 2) {
        ref_id[cmp_idx][ref_id_count[cmp_idx]] = candidate->mv[rf_idx];
        ++ref_id_count[cmp_idx];
#if CONFIG_NEW_REF_SIGNALING
      } else if (can_rf != INTRA_FRAME_NRS && can_rf != INVALID_IDX &&
                 ref_diff_count[cmp_idx] < 2) {
#else
      } else if (can_rf > INTRA_FRAME && ref_diff_count[cmp_idx] < 2) {
#endif  // CONFIG_NEW_REF_SIGNALING
        int_mv this_mv = candidate->mv[rf_idx];
#if CONFIG_NEW_REF_SIGNALING
        if (cm->ref_frame_sign_bias_nrs[can_rf] !=
            cm->ref_frame_sign_bias_nrs[rf[cmp_idx]])
#else
        if (cm->ref_frame_sign_bias[can_rf] !=
            cm->ref_frame_sign_bias[rf[cmp_idx]])
#endif  // CONFIG_NEW_REF_SIGNALING
        {
          this_mv.as_mv.row = -this_mv.as_mv.row;
          this_mv.as_mv.col = -this_mv.as_mv.col;
        }
        ref_diff[cmp_idx][ref_diff_count[cmp_idx]] = this_mv;
        ++ref_diff_count[cmp_idx];
      }
    }
  }
}

static AOM_INLINE void process_single_ref_mv_candidate(
    const MB_MODE_INFO *const candidate, const AV1_COMMON *const cm,
#if CONFIG_NEW_REF_SIGNALING
    MV_REFERENCE_FRAME_NRS ref_frame,
#else
    MV_REFERENCE_FRAME ref_frame,
#endif  // CONFIG_NEW_REF_SIGNALING
    uint8_t *const refmv_count,
    CANDIDATE_MV ref_mv_stack[MAX_REF_MV_STACK_SIZE],
    uint16_t ref_mv_weight[MAX_REF_MV_STACK_SIZE]) {
  for (int rf_idx = 0; rf_idx < 2; ++rf_idx) {
#if CONFIG_NEW_REF_SIGNALING
    if (candidate->ref_frame_nrs[rf_idx] != INTRA_FRAME_NRS &&
        candidate->ref_frame_nrs[rf_idx] != INVALID_IDX)
#else
    if (candidate->ref_frame[rf_idx] > INTRA_FRAME)
#endif  // CONFIG_NEW_REF_SIGNALING
    {
      int_mv this_mv = candidate->mv[rf_idx];
#if CONFIG_NEW_REF_SIGNALING
      if (cm->ref_frame_sign_bias_nrs[candidate->ref_frame_nrs[rf_idx]] !=
          cm->ref_frame_sign_bias_nrs[ref_frame]) {
        this_mv.as_mv.row = -this_mv.as_mv.row;
        this_mv.as_mv.col = -this_mv.as_mv.col;
      }
#else
      if (cm->ref_frame_sign_bias[candidate->ref_frame[rf_idx]] !=
          cm->ref_frame_sign_bias[ref_frame]) {
        this_mv.as_mv.row = -this_mv.as_mv.row;
        this_mv.as_mv.col = -this_mv.as_mv.col;
      }
#endif  // CONFIG_NEW_REF_SIGNALING
      int stack_idx;
      for (stack_idx = 0; stack_idx < *refmv_count; ++stack_idx) {
        const int_mv stack_mv = ref_mv_stack[stack_idx].this_mv;
        if (this_mv.as_int == stack_mv.as_int) break;
      }

      if (stack_idx == *refmv_count) {
        ref_mv_stack[stack_idx].this_mv = this_mv;

        // TODO(jingning): Set an arbitrary small number here. The weight
        // doesn't matter as long as it is properly initialized.
        ref_mv_weight[stack_idx] = 2;
        ++(*refmv_count);
#if CONFIG_NEW_INTER_MODES && NO_MV_PARSING_DEP
        if (*refmv_count >= MAX_MV_REF_CANDIDATES) return;
#endif  // CONFIG_NEW_INTER_MODES && NO_MV_PARSING_DEP
      }
    }
  }
}

#if CONFIG_REF_MV_BANK
static AOM_INLINE bool check_rmb_cand(CANDIDATE_MV cand_mv,
                                      CANDIDATE_MV *ref_mv_stack,
                                      uint16_t *ref_mv_weight, int refmv_count,
                                      int is_comp, int mi_row, int mi_col,
                                      int block_width, int block_height,
                                      int frame_width, int frame_height) {
  // Check if the MV candidate is already existing in the ref MV stack.
  int existing = 0;
  for (int i = 0; i < refmv_count; ++i) {
    if (ref_mv_stack[i].this_mv.as_int == cand_mv.this_mv.as_int &&
        (!is_comp ||
         ref_mv_stack[i].comp_mv.as_int == cand_mv.comp_mv.as_int)) {
      existing = 1;
      break;
    }
  }
  if (existing) return false;

  // Check if the MV candidate is pointing to ref block inside frame boundary.
  int mv_valid = 1;
  for (int i = 0; i < 1 + is_comp; ++i) {
    const int mv_row =
        (i ? cand_mv.comp_mv.as_mv.row : cand_mv.this_mv.as_mv.row) / 8;
    const int mv_col =
        (i ? cand_mv.comp_mv.as_mv.col : cand_mv.this_mv.as_mv.col) / 8;
    const int ref_x = mi_col * MI_SIZE + mv_col;
    const int ref_y = mi_row * MI_SIZE + mv_row;
    if (ref_x <= -block_width || ref_y <= -block_height ||
        ref_x >= frame_width || ref_y >= frame_height) {
      mv_valid = 0;
      break;
    }
  }
  if (!mv_valid) return false;

  ref_mv_stack[refmv_count] = cand_mv;
  ref_mv_weight[refmv_count] = REF_CAT_LEVEL;

  return true;
}
#endif  // CONFIG_REF_MV_BANK

static AOM_INLINE void setup_ref_mv_list(
    const AV1_COMMON *cm, const MACROBLOCKD *xd,
#if CONFIG_NEW_REF_SIGNALING
    MV_REFERENCE_FRAME_NRS ref_frame,
#else
    MV_REFERENCE_FRAME ref_frame,
#endif  // CONFIG_NEW_REF_SIGNALING
    uint8_t *const refmv_count,
    CANDIDATE_MV ref_mv_stack[MAX_REF_MV_STACK_SIZE],
    uint16_t ref_mv_weight[MAX_REF_MV_STACK_SIZE],
    int_mv mv_ref_list[MAX_MV_REF_CANDIDATES], int_mv *gm_mv_candidates,
    int mi_row, int mi_col, int16_t *mode_context) {
  const int has_tr = has_top_right(cm, xd, mi_row, mi_col, xd->width);

  const TileInfo *const tile = &xd->tile;
  int max_row_offset = 0, max_col_offset = 0;
  const int row_adj = (xd->height < mi_size_high[BLOCK_8X8]) && (mi_row & 0x01);
  const int col_adj = (xd->width < mi_size_wide[BLOCK_8X8]) && (mi_col & 0x01);
  int processed_rows = 0;
  int processed_cols = 0;

#if CONFIG_NEW_REF_SIGNALING
  MV_REFERENCE_FRAME_NRS rf[2];
  av1_set_ref_frame_nrs(rf, ref_frame);
#else
  MV_REFERENCE_FRAME rf[2];
  av1_set_ref_frame(rf, ref_frame);
#endif  // CONFIG_NEW_REF_SIGNALING

  mode_context[ref_frame] = 0;
  *refmv_count = 0;

  // Find valid maximum row/col offset.
  if (xd->up_available) {
    max_row_offset = -(MVREF_ROW_COLS << 1) + row_adj;

    if (xd->height < mi_size_high[BLOCK_8X8])
      max_row_offset = -(2 << 1) + row_adj;

    max_row_offset = find_valid_row_offset(tile, mi_row, max_row_offset);
  }

  if (xd->left_available) {
    max_col_offset = -(MVREF_ROW_COLS << 1) + col_adj;

    if (xd->width < mi_size_wide[BLOCK_8X8])
      max_col_offset = -(2 << 1) + col_adj;

    max_col_offset = find_valid_col_offset(tile, mi_col, max_col_offset);
  }

  uint8_t col_match_count = 0;
  uint8_t row_match_count = 0;
  uint8_t newmv_count = 0;

  // Scan the first above row mode info. row_offset = -1;
  if (abs(max_row_offset) >= 1)
    scan_row_mbmi(cm, xd,
#if CONFIG_EXT_RECUR_PARTITIONS
                  mi_row,
#endif  // CONFIG_EXT_RECUR_PARTITIONS
                  mi_col, rf, -1, ref_mv_stack, ref_mv_weight, refmv_count,
                  &row_match_count, &newmv_count, gm_mv_candidates,
                  max_row_offset, &processed_rows);
  // Scan the first left column mode info. col_offset = -1;
  if (abs(max_col_offset) >= 1)
    scan_col_mbmi(cm, xd, mi_row,
#if CONFIG_EXT_RECUR_PARTITIONS
                  mi_col,
#endif  // CONFIG_EXT_RECUR_PARTITIONS
                  rf, -1, ref_mv_stack, ref_mv_weight, refmv_count,
                  &col_match_count, &newmv_count, gm_mv_candidates,
                  max_col_offset, &processed_cols);
  // Check top-right boundary
  if (has_tr)
    scan_blk_mbmi(cm, xd, mi_row, mi_col, rf, -1, xd->width, ref_mv_stack,
                  ref_mv_weight, &row_match_count, &newmv_count,
                  gm_mv_candidates, refmv_count);

  const uint8_t nearest_match = (row_match_count > 0) + (col_match_count > 0);
  const uint8_t nearest_refmv_count = *refmv_count;

  // TODO(yunqing): for comp_search, do it for all 3 cases.
  for (int idx = 0; idx < nearest_refmv_count; ++idx)
    ref_mv_weight[idx] += REF_CAT_LEVEL;

  if (cm->features.allow_ref_frame_mvs) {
    int is_available = 0;
    const int voffset = AOMMAX(mi_size_high[BLOCK_8X8], xd->height);
    const int hoffset = AOMMAX(mi_size_wide[BLOCK_8X8], xd->width);
    const int blk_row_end = AOMMIN(xd->height, mi_size_high[BLOCK_64X64]);
    const int blk_col_end = AOMMIN(xd->width, mi_size_wide[BLOCK_64X64]);

    const int tpl_sample_pos[3][2] = {
      { voffset, -2 },
      { voffset, hoffset },
      { voffset - 2, hoffset },
    };
    const int allow_extension = (xd->height >= mi_size_high[BLOCK_8X8]) &&
                                (xd->height < mi_size_high[BLOCK_64X64]) &&
                                (xd->width >= mi_size_wide[BLOCK_8X8]) &&
                                (xd->width < mi_size_wide[BLOCK_64X64]);

    const int step_h = (xd->height >= mi_size_high[BLOCK_64X64])
                           ? mi_size_high[BLOCK_16X16]
                           : mi_size_high[BLOCK_8X8];
    const int step_w = (xd->width >= mi_size_wide[BLOCK_64X64])
                           ? mi_size_wide[BLOCK_16X16]
                           : mi_size_wide[BLOCK_8X8];

    for (int blk_row = 0; blk_row < blk_row_end; blk_row += step_h) {
      for (int blk_col = 0; blk_col < blk_col_end; blk_col += step_w) {
        int ret = add_tpl_ref_mv(cm, xd, mi_row, mi_col, ref_frame, blk_row,
                                 blk_col, gm_mv_candidates, refmv_count,
                                 ref_mv_stack, ref_mv_weight, mode_context);
        if (blk_row == 0 && blk_col == 0) is_available = ret;
      }
    }

    if (is_available == 0) mode_context[ref_frame] |= (1 << GLOBALMV_OFFSET);

    for (int i = 0; i < 3 && allow_extension; ++i) {
      const int blk_row = tpl_sample_pos[i][0];
      const int blk_col = tpl_sample_pos[i][1];

      if (!check_sb_border(mi_row, mi_col, blk_row, blk_col)) continue;
      add_tpl_ref_mv(cm, xd, mi_row, mi_col, ref_frame, blk_row, blk_col,
                     gm_mv_candidates, refmv_count, ref_mv_stack, ref_mv_weight,
                     mode_context);
    }
  }

  uint8_t dummy_newmv_count = 0;

  // Scan the second outer area.
  scan_blk_mbmi(cm, xd, mi_row, mi_col, rf, -1, -1, ref_mv_stack, ref_mv_weight,
                &row_match_count, &dummy_newmv_count, gm_mv_candidates,
                refmv_count);

  for (int idx = 2; idx <= MVREF_ROW_COLS; ++idx) {
    const int row_offset = -(idx << 1) + 1 + row_adj;
    const int col_offset = -(idx << 1) + 1 + col_adj;

    if (abs(row_offset) <= abs(max_row_offset) &&
        abs(row_offset) > processed_rows)
      scan_row_mbmi(cm, xd,
#if CONFIG_EXT_RECUR_PARTITIONS
                    mi_row,
#endif  // CONFIG_EXT_RECUR_PARTITIONS
                    mi_col, rf, row_offset, ref_mv_stack, ref_mv_weight,
                    refmv_count, &row_match_count, &dummy_newmv_count,
                    gm_mv_candidates, max_row_offset, &processed_rows);

    if (abs(col_offset) <= abs(max_col_offset) &&
        abs(col_offset) > processed_cols)
      scan_col_mbmi(cm, xd, mi_row,
#if CONFIG_EXT_RECUR_PARTITIONS
                    mi_col,
#endif  // CONFIG_EXT_RECUR_PARTITIONS
                    rf, col_offset, ref_mv_stack, ref_mv_weight, refmv_count,
                    &col_match_count, &dummy_newmv_count, gm_mv_candidates,
                    max_col_offset, &processed_cols);
  }

  const uint8_t ref_match_count = (row_match_count > 0) + (col_match_count > 0);

  switch (nearest_match) {
    case 0:
      if (ref_match_count >= 1) mode_context[ref_frame] |= 1;
      if (ref_match_count == 1)
        mode_context[ref_frame] |= (1 << REFMV_OFFSET);
      else if (ref_match_count >= 2)
        mode_context[ref_frame] |= (2 << REFMV_OFFSET);
      break;
    case 1:
      mode_context[ref_frame] |= (newmv_count > 0) ? 2 : 3;
      if (ref_match_count == 1)
        mode_context[ref_frame] |= (3 << REFMV_OFFSET);
      else if (ref_match_count >= 2)
        mode_context[ref_frame] |= (4 << REFMV_OFFSET);
      break;
    case 2:
    default:
      if (newmv_count >= 1)
        mode_context[ref_frame] |= 4;
      else
        mode_context[ref_frame] |= 5;

      mode_context[ref_frame] |= (5 << REFMV_OFFSET);
      break;
  }
#if CONFIG_NEW_INTER_MODES && NO_MV_PARSING_DEP
#if CONFIG_NEW_REF_SIGNALING
  int refmv_count_max_ctx =
      AOMMAX(*refmv_count, MAX_MV_REF_CANDIDATES) + (rf[1] == INVALID_IDX);
#else
  int refmv_count_max_ctx =
      AOMMAX(*refmv_count, MAX_MV_REF_CANDIDATES) + (rf[1] == NONE_FRAME);
#endif  // CONFIG_NEW_REF_SIGNALING
#if CONFIG_REF_MV_BANK
  refmv_count_max_ctx += xd->ref_mv_bank_left_pt->rmb_count[ref_frame];
#if REF_MV_BANK_COLS
  refmv_count_max_ctx +=
      xd->ref_mv_bank_above_pt[av1_get_column_bank_index(cm, xd->mi_col)]
          .rmb_count[ref_frame];
#endif  // REF_MV_BANK_COLS
#endif  // CONFIG_REF_MV_BANK
  refmv_count_max_ctx = AOMMIN(refmv_count_max_ctx, MAX_REF_MV_STACK_SIZE);
  // Add max #ref_mvs expected to second byte of mode_context
  mode_context[ref_frame] |= (refmv_count_max_ctx << 8);
#endif  // CONFIG_NEW_INTER_MODES && NO_MV_PARSING_DEP

  // Rank the likelihood and assign nearest and near mvs.
  int len = nearest_refmv_count;
  while (len > 0) {
    int nr_len = 0;
    for (int idx = 1; idx < len; ++idx) {
      if (ref_mv_weight[idx - 1] < ref_mv_weight[idx]) {
        const CANDIDATE_MV tmp_mv = ref_mv_stack[idx - 1];
        const uint16_t tmp_ref_mv_weight = ref_mv_weight[idx - 1];
        ref_mv_stack[idx - 1] = ref_mv_stack[idx];
        ref_mv_stack[idx] = tmp_mv;
        ref_mv_weight[idx - 1] = ref_mv_weight[idx];
        ref_mv_weight[idx] = tmp_ref_mv_weight;
        nr_len = idx;
      }
    }
    len = nr_len;
  }

  len = *refmv_count;
  while (len > nearest_refmv_count) {
    int nr_len = nearest_refmv_count;
    for (int idx = nearest_refmv_count + 1; idx < len; ++idx) {
      if (ref_mv_weight[idx - 1] < ref_mv_weight[idx]) {
        const CANDIDATE_MV tmp_mv = ref_mv_stack[idx - 1];
        const uint16_t tmp_ref_mv_weight = ref_mv_weight[idx - 1];
        ref_mv_stack[idx - 1] = ref_mv_stack[idx];
        ref_mv_stack[idx] = tmp_mv;
        ref_mv_weight[idx - 1] = ref_mv_weight[idx];
        ref_mv_weight[idx] = tmp_ref_mv_weight;
        nr_len = idx;
      }
    }
    len = nr_len;
  }

  int mi_width = AOMMIN(mi_size_wide[BLOCK_64X64], xd->width);
  mi_width = AOMMIN(mi_width, cm->mi_params.mi_cols - mi_col);
  int mi_height = AOMMIN(mi_size_high[BLOCK_64X64], xd->height);
  mi_height = AOMMIN(mi_height, cm->mi_params.mi_rows - mi_row);
  const int mi_size = AOMMIN(mi_width, mi_height);
#if CONFIG_NEW_REF_SIGNALING
  if (rf[1] > INVALID_IDX) {
#else
  if (rf[1] > NONE_FRAME) {
#endif  // CONFIG_NEW_REF_SIGNALING
    // TODO(jingning, yunqing): Refactor and consolidate the compound and
    // single reference frame modes. Reduce unnecessary redundancy.
    if (*refmv_count < MAX_MV_REF_CANDIDATES) {
      int_mv ref_id[2][2], ref_diff[2][2];
      int ref_id_count[2] = { 0 }, ref_diff_count[2] = { 0 };

      for (int idx = 0; abs(max_row_offset) >= 1 && idx < mi_size;) {
        const MB_MODE_INFO *const candidate = xd->mi[-xd->mi_stride + idx];
        process_compound_ref_mv_candidate(
            candidate, cm, rf, ref_id, ref_id_count, ref_diff, ref_diff_count);
        idx += mi_size_wide[candidate->sb_type];
      }

      for (int idx = 0; abs(max_col_offset) >= 1 && idx < mi_size;) {
        const MB_MODE_INFO *const candidate = xd->mi[idx * xd->mi_stride - 1];
        process_compound_ref_mv_candidate(
            candidate, cm, rf, ref_id, ref_id_count, ref_diff, ref_diff_count);
        idx += mi_size_high[candidate->sb_type];
      }

      // Build up the compound mv predictor
      int_mv comp_list[MAX_MV_REF_CANDIDATES][2];

      for (int idx = 0; idx < 2; ++idx) {
        int comp_idx = 0;
        for (int list_idx = 0;
             list_idx < ref_id_count[idx] && comp_idx < MAX_MV_REF_CANDIDATES;
             ++list_idx, ++comp_idx)
          comp_list[comp_idx][idx] = ref_id[idx][list_idx];
        for (int list_idx = 0;
             list_idx < ref_diff_count[idx] && comp_idx < MAX_MV_REF_CANDIDATES;
             ++list_idx, ++comp_idx)
          comp_list[comp_idx][idx] = ref_diff[idx][list_idx];
        for (; comp_idx < MAX_MV_REF_CANDIDATES; ++comp_idx)
          comp_list[comp_idx][idx] = gm_mv_candidates[idx];
      }

      if (*refmv_count) {
        assert(*refmv_count == 1);
        if (comp_list[0][0].as_int == ref_mv_stack[0].this_mv.as_int &&
            comp_list[0][1].as_int == ref_mv_stack[0].comp_mv.as_int) {
          ref_mv_stack[*refmv_count].this_mv = comp_list[1][0];
          ref_mv_stack[*refmv_count].comp_mv = comp_list[1][1];
        } else {
          ref_mv_stack[*refmv_count].this_mv = comp_list[0][0];
          ref_mv_stack[*refmv_count].comp_mv = comp_list[0][1];
        }
        ref_mv_weight[*refmv_count] = 2;
        ++*refmv_count;
      } else {
        for (int idx = 0; idx < MAX_MV_REF_CANDIDATES; ++idx) {
          ref_mv_stack[*refmv_count].this_mv = comp_list[idx][0];
          ref_mv_stack[*refmv_count].comp_mv = comp_list[idx][1];
          ref_mv_weight[*refmv_count] = 2;
          ++*refmv_count;
        }
      }
    }

    assert(*refmv_count >= MAX_MV_REF_CANDIDATES);

    for (int idx = 0; idx < *refmv_count; ++idx) {
      clamp_mv_ref(&ref_mv_stack[idx].this_mv.as_mv, xd->width << MI_SIZE_LOG2,
                   xd->height << MI_SIZE_LOG2, xd);
      clamp_mv_ref(&ref_mv_stack[idx].comp_mv.as_mv, xd->width << MI_SIZE_LOG2,
                   xd->height << MI_SIZE_LOG2, xd);
    }
  } else {
    // Handle single reference frame extension
    for (int idx = 0; abs(max_row_offset) >= 1 && idx < mi_size &&
                      *refmv_count < MAX_MV_REF_CANDIDATES;) {
      const MB_MODE_INFO *const candidate = xd->mi[-xd->mi_stride + idx];
      process_single_ref_mv_candidate(candidate, cm, ref_frame, refmv_count,
                                      ref_mv_stack, ref_mv_weight);
      idx += mi_size_wide[candidate->sb_type];
    }

    for (int idx = 0; abs(max_col_offset) >= 1 && idx < mi_size &&
                      *refmv_count < MAX_MV_REF_CANDIDATES;) {
      const MB_MODE_INFO *const candidate = xd->mi[idx * xd->mi_stride - 1];
      process_single_ref_mv_candidate(candidate, cm, ref_frame, refmv_count,
                                      ref_mv_stack, ref_mv_weight);
      idx += mi_size_high[candidate->sb_type];
    }

    for (int idx = 0; idx < *refmv_count; ++idx) {
      clamp_mv_ref(&ref_mv_stack[idx].this_mv.as_mv, xd->width << MI_SIZE_LOG2,
                   xd->height << MI_SIZE_LOG2, xd);
    }

    if (mv_ref_list != NULL) {
      for (int idx = *refmv_count; idx < MAX_MV_REF_CANDIDATES; ++idx)
        mv_ref_list[idx].as_int = gm_mv_candidates[0].as_int;

      for (int idx = 0; idx < AOMMIN(MAX_MV_REF_CANDIDATES, *refmv_count);
           ++idx) {
        mv_ref_list[idx].as_int = ref_mv_stack[idx].this_mv.as_int;
      }
    }
#if CONFIG_NEW_INTER_MODES
    // If there is extra space in the stack, copy the GLOBALMV vector into it.
    // This also guarantees the existence of at least one vector to search.
    if (*refmv_count < MAX_REF_MV_STACK_SIZE) {
      int stack_idx;
      for (stack_idx = 0; stack_idx < *refmv_count; ++stack_idx) {
        const int_mv stack_mv = ref_mv_stack[stack_idx].this_mv;
        if (gm_mv_candidates[0].as_int == stack_mv.as_int) break;
      }
      if (stack_idx == *refmv_count) {
        ref_mv_stack[*refmv_count].this_mv.as_int = gm_mv_candidates[0].as_int;
        ref_mv_stack[*refmv_count].comp_mv.as_int = gm_mv_candidates[1].as_int;
        ref_mv_weight[*refmv_count] = REF_CAT_LEVEL;
        (*refmv_count)++;
      }
    }
#endif  // CONFIG_NEW_INTER_MODES
  }
#if CONFIG_NEW_INTER_MODES && NO_MV_PARSING_DEP
  assert(*refmv_count <= refmv_count_max_ctx);
#endif  // CONFIG_NEW_INTER_MODES && NO_MV_PARSING_DEP

#if CONFIG_REF_MV_BANK
  // TODO(huisu): do we need the AOMMIN? can we just use the first ?
#if CONFIG_NEW_INTER_MODES
  const int ref_mv_limit =
      AOMMIN(cm->features.max_drl_bits + 1, MAX_REF_MV_STACK_SIZE);
#else
  const int ref_mv_limit =
      AOMMIN(USABLE_REF_MV_STACK_SIZE, MAX_REF_MV_STACK_SIZE);
#endif  // CONFIG_NEW_INTER_MODES
  // If open slots are available, fetch reference MVs from the ref mv banks.
  if (*refmv_count < ref_mv_limit &&
#if CONFIG_NEW_REF_SIGNALING
      ref_frame != INTRA_FRAME_NRS
#else
      ref_frame != INTRA_FRAME
#endif  // CONFIG_NEW_REF_SIGNALING
  ) {
    const REF_MV_BANK *ref_mv_bank_left = xd->ref_mv_bank_left_pt;
    const CANDIDATE_MV *queue_left = ref_mv_bank_left->rmb_buffer[ref_frame];
    const int count_left = ref_mv_bank_left->rmb_count[ref_frame];
    const int start_idx_left = ref_mv_bank_left->rmb_start_idx[ref_frame];
    int idx_left = 0;
#if REF_MV_BANK_COLS
    const int col_bank_idx = av1_get_column_bank_index(cm, xd->mi_col);
    // const int col_bank_idx = xd->mi_col / cm->seq_params.mib_size;
    const REF_MV_BANK *ref_mv_bank_above =
        &xd->ref_mv_bank_above_pt[col_bank_idx];
    const int count_above = ref_mv_bank_above->rmb_count[ref_frame];
    const CANDIDATE_MV *queue_above = ref_mv_bank_above->rmb_buffer[ref_frame];
    const int start_idx_above = ref_mv_bank_above->rmb_start_idx[ref_frame];
    int idx_above = 0;
#endif  // REF_MV_BANK_COLS
#if CONFIG_NEW_REF_SIGNALING
    const int is_comp = rf[1] != INTRA_FRAME_NRS && rf[1] != INVALID_IDX;
#else
    const int is_comp = rf[1] > INTRA_FRAME;
#endif  // CONFIG_NEW_REF_SIGNALING
    const int block_width = xd->width * MI_SIZE;
    const int block_height = xd->height * MI_SIZE;

    do {
      for (; idx_left < count_left && *refmv_count < ref_mv_limit; ++idx_left) {
        const int idx =
            (start_idx_left + count_left - 1 - idx_left) % REF_MV_BANK_SIZE;
        const CANDIDATE_MV cand_mv = queue_left[idx];
        if (check_rmb_cand(cand_mv, ref_mv_stack, ref_mv_weight, *refmv_count,
                           is_comp, xd->mi_row, xd->mi_col, block_width,
                           block_height, cm->width, cm->height)) {
          ++*refmv_count;
          break;
        }
      }

#if REF_MV_BANK_COLS
      for (; idx_above < count_above && *refmv_count < ref_mv_limit;
           ++idx_above) {
        const int idx =
            (start_idx_above + count_above - 1 - idx_above) % REF_MV_BANK_SIZE;
        const CANDIDATE_MV cand_mv = queue_above[idx];
        if (check_rmb_cand(cand_mv, ref_mv_stack, ref_mv_weight, *refmv_count,
                           is_comp, xd->mi_row, xd->mi_col, block_width,
                           block_height, cm->width, cm->height)) {
          ++*refmv_count;
          break;
        }
      }

      if (idx_left >= count_left && idx_above >= count_above) break;
#else
      if (idx_left >= count_left) break;
#endif  // REF_MV_BANK_COLS
    } while (*refmv_count < ref_mv_limit);

#if !CONFIG_NEW_INTER_MODES
    if (mv_ref_list != NULL) {
      for (int idx = *refmv_count; idx < MAX_MV_REF_CANDIDATES; ++idx)
        mv_ref_list[idx].as_int = gm_mv_candidates[0].as_int;

      for (int idx = 0; idx < AOMMIN(MAX_MV_REF_CANDIDATES, *refmv_count);
           ++idx) {
        mv_ref_list[idx].as_int = ref_mv_stack[idx].this_mv.as_int;
      }
    }
#endif  // !CONFIG_NEW_INTER_MODES
  }
#endif  // CONFIG_REF_MV_BANK
}

#if CONFIG_NEW_REF_SIGNALING
void av1_find_mv_refs_nrs(const AV1_COMMON *cm, const MACROBLOCKD *xd,
                          MB_MODE_INFO *mi,
                          MV_REFERENCE_FRAME_NRS ref_frame_nrs,
                          uint8_t ref_mv_count[MODE_CTX_REF_FRAMES],
                          CANDIDATE_MV ref_mv_stack[][MAX_REF_MV_STACK_SIZE],
                          uint16_t ref_mv_weight[][MAX_REF_MV_STACK_SIZE],
                          int_mv mv_ref_list[][MAX_MV_REF_CANDIDATES],
                          int_mv *global_mvs, int16_t *mode_context) {
  const int mi_row = xd->mi_row;
  const int mi_col = xd->mi_col;
  int_mv gm_mv[2];

  if (ref_frame_nrs == INTRA_FRAME_NRS) {
    gm_mv[0].as_int = gm_mv[1].as_int = 0;
  } else {
    const BLOCK_SIZE bsize = mi->sb_type;
    const int fr_mv_precision = cm->features.fr_mv_precision;
    if (ref_frame_nrs < INTER_REFS_PER_FRAME_NRS) {
      gm_mv[0] = gm_get_motion_vector(&cm->global_motion_nrs[ref_frame_nrs],
                                      fr_mv_precision, bsize, mi_col, mi_row);
      gm_mv[1].as_int = 0;
      if (global_mvs != NULL) global_mvs[ref_frame_nrs] = gm_mv[0];
    } else {
      MV_REFERENCE_FRAME_NRS rf[2];
      av1_set_ref_frame_nrs(rf, ref_frame_nrs);
      gm_mv[0] = gm_get_motion_vector(&cm->global_motion_nrs[rf[0]],
                                      fr_mv_precision, bsize, mi_col, mi_row);
      gm_mv[1] = gm_get_motion_vector(&cm->global_motion_nrs[rf[1]],
                                      fr_mv_precision, bsize, mi_col, mi_row);
    }
  }
  setup_ref_mv_list(cm, xd, ref_frame_nrs, &ref_mv_count[ref_frame_nrs],
                    ref_mv_stack[ref_frame_nrs], ref_mv_weight[ref_frame_nrs],
                    mv_ref_list ? mv_ref_list[ref_frame_nrs] : NULL, gm_mv,
                    mi_row, mi_col, mode_context);
}
#else
void av1_find_mv_refs(const AV1_COMMON *cm, const MACROBLOCKD *xd,
                      MB_MODE_INFO *mi, MV_REFERENCE_FRAME ref_frame,
                      uint8_t ref_mv_count[MODE_CTX_REF_FRAMES],
                      CANDIDATE_MV ref_mv_stack[][MAX_REF_MV_STACK_SIZE],
                      uint16_t ref_mv_weight[][MAX_REF_MV_STACK_SIZE],
                      int_mv mv_ref_list[][MAX_MV_REF_CANDIDATES],
                      int_mv *global_mvs, int16_t *mode_context) {
  const int mi_row = xd->mi_row;
  const int mi_col = xd->mi_col;
  int_mv gm_mv[2];

  if (ref_frame == INTRA_FRAME) {
    gm_mv[0].as_int = gm_mv[1].as_int = 0;
  } else {
    const BLOCK_SIZE bsize = mi->sb_type;
    const int fr_mv_precision = cm->features.fr_mv_precision;
    if (ref_frame < REF_FRAMES) {
      gm_mv[0] = gm_get_motion_vector(&cm->global_motion[ref_frame],
                                      fr_mv_precision, bsize, mi_col, mi_row);
      gm_mv[1].as_int = 0;
      if (global_mvs != NULL) global_mvs[ref_frame] = gm_mv[0];
    } else {
      MV_REFERENCE_FRAME rf[2];
      av1_set_ref_frame(rf, ref_frame);
      gm_mv[0] = gm_get_motion_vector(&cm->global_motion[rf[0]],
                                      fr_mv_precision, bsize, mi_col, mi_row);
      gm_mv[1] = gm_get_motion_vector(&cm->global_motion[rf[1]],
                                      fr_mv_precision, bsize, mi_col, mi_row);
    }
  }

  setup_ref_mv_list(cm, xd, ref_frame, &ref_mv_count[ref_frame],
                    ref_mv_stack[ref_frame], ref_mv_weight[ref_frame],
                    mv_ref_list ? mv_ref_list[ref_frame] : NULL, gm_mv, mi_row,
                    mi_col, mode_context);
}
#endif  // CONFIG_NEW_REF_SIGNALING

void av1_find_best_ref_mvs(int_mv *mvlist, int_mv *nearest_mv, int_mv *near_mv,
                           MvSubpelPrecision precision) {
  int i;
  // Make sure all the candidates are properly clamped etc
  for (i = 0; i < MAX_MV_REF_CANDIDATES; ++i) {
    lower_mv_precision(&mvlist[i].as_mv, precision);
  }
  *nearest_mv = mvlist[0];
  *near_mv = mvlist[1];
}

void av1_setup_frame_buf_refs(AV1_COMMON *cm) {
  cm->cur_frame->order_hint = cm->current_frame.order_hint;
  cm->cur_frame->display_order_hint = cm->current_frame.display_order_hint;
  cm->cur_frame->absolute_poc = cm->current_frame.absolute_poc;
  cm->cur_frame->pyramid_level = cm->current_frame.pyramid_level;

#if CONFIG_NEW_REF_SIGNALING
  MV_REFERENCE_FRAME_NRS ref_frame_nrs;
  for (ref_frame_nrs = 0; ref_frame_nrs < INTER_REFS_PER_FRAME_NRS;
       ++ref_frame_nrs) {
    const RefCntBuffer *const buf = get_ref_frame_buf_nrs(cm, ref_frame_nrs);
    if (buf != NULL && ref_frame_nrs < cm->new_ref_frame_data.n_total_refs) {
      cm->cur_frame->ref_order_hints_nrs[ref_frame_nrs] = buf->order_hint;
      cm->cur_frame->ref_display_order_hint_nrs[ref_frame_nrs] =
          buf->display_order_hint;
    } else {
      cm->cur_frame->ref_order_hints_nrs[ref_frame_nrs] = -1;
      cm->cur_frame->ref_display_order_hint_nrs[ref_frame_nrs] = -1;
    }
  }
#else
  MV_REFERENCE_FRAME ref_frame;
  for (ref_frame = LAST_FRAME; ref_frame <= ALTREF_FRAME; ++ref_frame) {
    const RefCntBuffer *const buf = get_ref_frame_buf(cm, ref_frame);
    if (buf != NULL) {
      cm->cur_frame->ref_order_hints[ref_frame - LAST_FRAME] = buf->order_hint;
      cm->cur_frame->ref_display_order_hint[ref_frame - LAST_FRAME] =
          buf->display_order_hint;
    }
  }
#endif  // CONFIG_NEW_REF_SIGNALING
}

void av1_setup_frame_sign_bias(AV1_COMMON *cm) {
#if CONFIG_NEW_REF_SIGNALING
  memset(&cm->ref_frame_sign_bias_nrs, 0, sizeof(cm->ref_frame_sign_bias_nrs));
  for (int ref_frame = 0; ref_frame < cm->new_ref_frame_data.n_future_refs;
       ++ref_frame) {
    const int index = cm->new_ref_frame_data.future_refs[ref_frame];
    cm->ref_frame_sign_bias_nrs[index] = 1;
  }
#else
  memset(&cm->ref_frame_sign_bias, 0, sizeof(cm->ref_frame_sign_bias));
  MV_REFERENCE_FRAME ref_frame;
  for (ref_frame = LAST_FRAME; ref_frame <= ALTREF_FRAME; ++ref_frame) {
    const RefCntBuffer *const buf = get_ref_frame_buf(cm, ref_frame);
    if (cm->seq_params.order_hint_info.enable_order_hint && buf != NULL) {
      const int ref_order_hint = buf->order_hint;
      cm->ref_frame_sign_bias[ref_frame] =
          (get_relative_dist(&cm->seq_params.order_hint_info, ref_order_hint,
                             (int)cm->current_frame.order_hint) <= 0)
              ? 0
              : 1;
    } else {
      cm->ref_frame_sign_bias[ref_frame] = 0;
    }
  }
#endif  // CONFIG_NEW_REF_SIGNALING
}

#define MAX_OFFSET_WIDTH 64
#define MAX_OFFSET_HEIGHT 0

static int get_block_position(AV1_COMMON *cm, int *mi_r, int *mi_c, int blk_row,
                              int blk_col, MV mv, int sign_bias) {
  const int base_blk_row = (blk_row >> 3) << 3;
  const int base_blk_col = (blk_col >> 3) << 3;

  const int row_offset = (mv.row >= 0) ? (mv.row >> (4 + MI_SIZE_LOG2))
                                       : -((-mv.row) >> (4 + MI_SIZE_LOG2));

  const int col_offset = (mv.col >= 0) ? (mv.col >> (4 + MI_SIZE_LOG2))
                                       : -((-mv.col) >> (4 + MI_SIZE_LOG2));

  const int row =
      (sign_bias == 1) ? blk_row - row_offset : blk_row + row_offset;
  const int col =
      (sign_bias == 1) ? blk_col - col_offset : blk_col + col_offset;

  if (row < 0 || row >= (cm->mi_params.mi_rows >> 1) || col < 0 ||
      col >= (cm->mi_params.mi_cols >> 1))
    return 0;

  if (row < base_blk_row - (MAX_OFFSET_HEIGHT >> 3) ||
      row >= base_blk_row + 8 + (MAX_OFFSET_HEIGHT >> 3) ||
      col < base_blk_col - (MAX_OFFSET_WIDTH >> 3) ||
      col >= base_blk_col + 8 + (MAX_OFFSET_WIDTH >> 3))
    return 0;

  *mi_r = row;
  *mi_c = col;

  return 1;
}

static int is_ref_motion_field_eligible(
    const AV1_COMMON *const cm, const RefCntBuffer *const start_frame_buf) {
  if (start_frame_buf == NULL) return 0;

  if (start_frame_buf->frame_type == KEY_FRAME ||
      start_frame_buf->frame_type == INTRA_ONLY_FRAME)
    return 0;

  if (start_frame_buf->mi_rows != cm->mi_params.mi_rows ||
      start_frame_buf->mi_cols != cm->mi_params.mi_cols)
    return 0;
  return 1;
}

// Note: motion_filed_projection finds motion vectors of current frame's
// reference frame, and projects them to current frame. To make it clear,
// let's call current frame's reference frame as start frame.
// Call Start frame's reference frames as reference frames.
// Call ref_offset as frame distances between start frame and its reference
// frames.
static int motion_field_projection(AV1_COMMON *cm,
#if CONFIG_NEW_REF_SIGNALING
                                   MV_REFERENCE_FRAME_NRS start_frame,
#else
                                   MV_REFERENCE_FRAME start_frame,
#endif  // CONFIG_NEW_REF_SIGNALING
                                   int dir, int overwrite) {
  TPL_MV_REF *tpl_mvs_base = cm->tpl_mvs;
#if CONFIG_NEW_REF_SIGNALING
  int ref_offset[INTER_REFS_PER_FRAME_NRS] = { 0 };

  const RefCntBuffer *const start_frame_buf =
      get_ref_frame_buf_nrs(cm, start_frame);
#else
  int ref_offset[REF_FRAMES] = { 0 };

  const RefCntBuffer *const start_frame_buf =
      get_ref_frame_buf(cm, start_frame);
#endif  // CONFIG_NEW_REF_SIGNALING
  if (!is_ref_motion_field_eligible(cm, start_frame_buf)) return 0;

  const int start_frame_order_hint = start_frame_buf->order_hint;
  const int cur_order_hint = cm->cur_frame->order_hint;
  int start_to_current_frame_offset = get_relative_dist(
      &cm->seq_params.order_hint_info, start_frame_order_hint, cur_order_hint);

#if CONFIG_NEW_REF_SIGNALING
  const int *const ref_order_hints = &start_frame_buf->ref_order_hints_nrs[0];
  int valid_ref = 0;
  for (MV_REFERENCE_FRAME_NRS rf = 0; rf < INTER_REFS_PER_FRAME_NRS; ++rf) {
    if (ref_order_hints[rf] != -1) {
      ref_offset[rf] =
          get_relative_dist(&cm->seq_params.order_hint_info,
                            start_frame_order_hint, ref_order_hints[rf]);
      valid_ref = 1;
    }
  }
  (void)valid_ref;
  assert(valid_ref);
#else
  const unsigned int *const ref_order_hints =
      &start_frame_buf->ref_order_hints[0];
  for (MV_REFERENCE_FRAME rf = LAST_FRAME; rf <= INTER_REFS_PER_FRAME; ++rf) {
    ref_offset[rf] = get_relative_dist(&cm->seq_params.order_hint_info,
                                       start_frame_order_hint,
                                       ref_order_hints[rf - LAST_FRAME]);
  }
#endif  // CONFIG_NEW_REF_SIGNALING

  if (dir == 2) start_to_current_frame_offset = -start_to_current_frame_offset;

  MV_REF *mv_ref_base = start_frame_buf->mvs;
  const int mvs_rows = (cm->mi_params.mi_rows + 1) >> 1;
  const int mvs_cols = (cm->mi_params.mi_cols + 1) >> 1;

  for (int blk_row = 0; blk_row < mvs_rows; ++blk_row) {
    for (int blk_col = 0; blk_col < mvs_cols; ++blk_col) {
      MV_REF *mv_ref = &mv_ref_base[blk_row * mvs_cols + blk_col];
      MV fwd_mv = mv_ref->mv.as_mv;
#if CONFIG_NEW_REF_SIGNALING
      if (mv_ref->ref_frame != INTRA_FRAME_NRS &&
          mv_ref->ref_frame != INVALID_IDX) {
#else
      if (mv_ref->ref_frame > INTRA_FRAME) {
#endif  // CONFIG_NEW_REF_SIGNALING
        int_mv this_mv;
        int mi_r, mi_c;
        const int ref_frame_offset = ref_offset[mv_ref->ref_frame];

        int pos_valid =
            abs(ref_frame_offset) <= MAX_FRAME_DISTANCE &&
            ref_frame_offset > 0 &&
            abs(start_to_current_frame_offset) <= MAX_FRAME_DISTANCE;

        if (pos_valid) {
          get_mv_projection(&this_mv.as_mv, fwd_mv,
                            start_to_current_frame_offset, ref_frame_offset);
          pos_valid = get_block_position(cm, &mi_r, &mi_c, blk_row, blk_col,
                                         this_mv.as_mv, dir >> 1);
        }

        if (pos_valid) {
          const int mi_offset = mi_r * (cm->mi_params.mi_stride >> 1) + mi_c;
          if (overwrite || tpl_mvs_base[mi_offset].mfmv0.as_int == INVALID_MV) {
            tpl_mvs_base[mi_offset].mfmv0.as_mv.row = fwd_mv.row;
            tpl_mvs_base[mi_offset].mfmv0.as_mv.col = fwd_mv.col;
            tpl_mvs_base[mi_offset].ref_frame_offset = ref_frame_offset;
          }
        }
      }
    }
  }

  return 1;
}

#if CONFIG_NEW_REF_SIGNALING
static INLINE int is_ref_overlay_nrs(const AV1_COMMON *const cm, int ref) {
  const OrderHintInfo *const order_hint_info = &cm->seq_params.order_hint_info;
  if (!order_hint_info->enable_order_hint) return -1;
  const RefCntBuffer *const buf = get_ref_frame_buf_nrs(cm, ref);
  if (buf == NULL) return -1;
  const int ref_order_hint = buf->order_hint;
  for (int r = 0; r < INTER_REFS_PER_FRAME_NRS; ++r) {
    if (buf->ref_order_hints_nrs[r] == -1) continue;
    const int ref_ref_order_hint = buf->ref_order_hints_nrs[r];
    if (get_relative_dist(order_hint_info, ref_order_hint,
                          ref_ref_order_hint) == 0)
      return 1;
  }
  return 0;
}
#endif  // CONFIG_NEW_REF_SIGNALING

void av1_setup_motion_field(AV1_COMMON *cm) {
  const OrderHintInfo *const order_hint_info = &cm->seq_params.order_hint_info;

#if CONFIG_NEW_REF_SIGNALING
  memset(cm->ref_frame_side_nrs, 0, sizeof(cm->ref_frame_side_nrs));
#else
  memset(cm->ref_frame_side, 0, sizeof(cm->ref_frame_side));
#endif  // CONFIG_NEW_REF_SIGNALING
  if (!order_hint_info->enable_order_hint) return;

  TPL_MV_REF *tpl_mvs_base = cm->tpl_mvs;
  int size = ((cm->mi_params.mi_rows + MAX_MIB_SIZE) >> 1) *
             (cm->mi_params.mi_stride >> 1);
  for (int idx = 0; idx < size; ++idx) {
    tpl_mvs_base[idx].mfmv0.as_int = INVALID_MV;
    tpl_mvs_base[idx].ref_frame_offset = 0;
  }

  const RefCntBuffer *ref_buf[INTER_REFS_PER_FRAME] = { 0 };
  int ref_order_hint[INTER_REFS_PER_FRAME];
  (void)ref_buf;
  (void)ref_order_hint;

#if CONFIG_NEW_REF_SIGNALING
  for (int index = 0; index < cm->new_ref_frame_data.n_past_refs; index++) {
    const int ref_frame = cm->new_ref_frame_data.past_refs[index];
    cm->ref_frame_side_nrs[ref_frame] = 0;
    const RefCntBuffer *const buf = get_ref_frame_buf_nrs(cm, ref_frame);
    int order_hint = 0;
    if (buf != NULL) order_hint = buf->order_hint;
    ref_buf[ref_frame] = buf;
    ref_order_hint[ref_frame] = order_hint;
  }
  for (int index = 0; index < cm->new_ref_frame_data.n_future_refs; index++) {
    const int ref_frame = cm->new_ref_frame_data.future_refs[index];
    cm->ref_frame_side_nrs[ref_frame] = 1;
    const RefCntBuffer *const buf = get_ref_frame_buf_nrs(cm, ref_frame);
    int order_hint = 0;
    if (buf != NULL) order_hint = buf->order_hint;
    ref_buf[ref_frame] = buf;
    ref_order_hint[ref_frame] = order_hint;
  }
  for (int index = 0; index < cm->new_ref_frame_data.n_cur_refs; index++) {
    const int ref_frame = cm->new_ref_frame_data.cur_refs[index];
    cm->ref_frame_side_nrs[ref_frame] = -1;
    const RefCntBuffer *const buf = get_ref_frame_buf_nrs(cm, ref_frame);
    int order_hint = 0;
    if (buf != NULL) order_hint = buf->order_hint;
    ref_buf[ref_frame] = buf;
    ref_order_hint[ref_frame] = order_hint;
  }
#else
  const int cur_order_hint = cm->cur_frame->order_hint;

  for (int ref_frame = LAST_FRAME; ref_frame <= ALTREF_FRAME; ref_frame++) {
    const int ref_idx = ref_frame - LAST_FRAME;
    const RefCntBuffer *const buf = get_ref_frame_buf(cm, ref_frame);
    int order_hint = 0;

    if (buf != NULL) order_hint = buf->order_hint;

    ref_buf[ref_idx] = buf;
    ref_order_hint[ref_idx] = order_hint;

    if (get_relative_dist(order_hint_info, order_hint, cur_order_hint) > 0)
      cm->ref_frame_side[ref_frame] = 1;
    else if (order_hint == cur_order_hint)
      cm->ref_frame_side[ref_frame] = -1;
  }
#endif  // CONFIG_NEW_REF_SIGNALING

#if CONFIG_NEW_REF_SIGNALING
  int n_refs_used = 0;

  // Implements a strategy where the closest references in the past
  // and future ranked lists are processed first, followed by
  // processing the second closest references up to MFMV_STACK_SIZE.
  //
  // Find two closest past and future references
  int dist[2][2] = { { INT_MAX, INT_MAX }, { INT_MAX, INT_MAX } };
  int closest_ref[2][2] = { { -1, -1 }, { -1, -1 } };
  for (int ref_frame = 0; ref_frame < cm->new_ref_frame_data.n_total_refs;
       ref_frame++) {
    const int dir = cm->ref_frame_side_nrs[ref_frame];
    if (dir == -1 || is_ref_overlay_nrs(cm, ref_frame) ||
        !is_ref_motion_field_eligible(cm, ref_buf[ref_frame]))
      continue;
    const int absdist =
        abs(cm->new_ref_frame_data.ref_frame_distance[ref_frame]);
    if (absdist < dist[dir][0]) {
      dist[dir][1] = dist[dir][0];
      closest_ref[dir][1] = closest_ref[dir][0];
      dist[dir][0] = absdist;
      closest_ref[dir][0] = ref_frame;
    } else if (absdist < dist[dir][1]) {
      dist[dir][1] = absdist;
      closest_ref[dir][1] = ref_frame;
    }
  }
  // Do projection on closest past and future refs if they exist
  if (closest_ref[0][0] != -1) {
    const int ret = motion_field_projection(cm, closest_ref[0][0], 2, 1);
    n_refs_used += ret;
  }
  if (closest_ref[1][0] != -1) {
    const int ret = motion_field_projection(cm, closest_ref[1][0], 0, 1);
    n_refs_used += ret;
  }
  // Add second closest from future and past if there are fewer than
  // MFMV_STACK_SIZE frames processed so far.
  if (closest_ref[1][1] != -1 && n_refs_used < MFMV_STACK_SIZE) {
    const int ret = motion_field_projection(cm, closest_ref[1][1], 0, 0);
    n_refs_used += ret;
  }
  if (closest_ref[0][1] != -1 && n_refs_used < MFMV_STACK_SIZE) {
    const int ret = motion_field_projection(cm, closest_ref[0][1], 2, 0);
    n_refs_used += ret;
  }
#else
  int ref_stamp = MFMV_STACK_SIZE - 1;

  if (ref_buf[LAST_FRAME - LAST_FRAME] != NULL) {
    const int alt_of_lst_order_hint =
        ref_buf[LAST_FRAME - LAST_FRAME]
            ->ref_order_hints[ALTREF_FRAME - LAST_FRAME];

    const int is_lst_overlay =
        (alt_of_lst_order_hint == ref_order_hint[GOLDEN_FRAME - LAST_FRAME]);
    if (!is_lst_overlay) motion_field_projection(cm, LAST_FRAME, 2, 1);
    --ref_stamp;
  }

  if (get_relative_dist(order_hint_info,
                        ref_order_hint[BWDREF_FRAME - LAST_FRAME],
                        cur_order_hint) > 0) {
    if (motion_field_projection(cm, BWDREF_FRAME, 0, 1)) --ref_stamp;
  }

  if (get_relative_dist(order_hint_info,
                        ref_order_hint[ALTREF2_FRAME - LAST_FRAME],
                        cur_order_hint) > 0) {
    if (motion_field_projection(cm, ALTREF2_FRAME, 0, 1)) --ref_stamp;
  }

  if (get_relative_dist(order_hint_info,
                        ref_order_hint[ALTREF_FRAME - LAST_FRAME],
                        cur_order_hint) > 0 &&
      ref_stamp >= 0)
    if (motion_field_projection(cm, ALTREF_FRAME, 0, 1)) --ref_stamp;

  if (ref_stamp >= 0) motion_field_projection(cm, LAST2_FRAME, 2, 1);
#endif  // CONFIG_NEW_REF_SIGNALING
}

static INLINE void record_samples(const MB_MODE_INFO *mbmi,
#if CONFIG_COMPOUND_WARP_SAMPLES
                                  int ref,
#endif  // CONFIG_COMPOUND_WARP_SAMPLES
                                  int *pts, int *pts_inref, int row_offset,
                                  int sign_r, int col_offset, int sign_c) {
  int bw = block_size_wide[mbmi->sb_type];
  int bh = block_size_high[mbmi->sb_type];
  int x = col_offset * MI_SIZE + sign_c * AOMMAX(bw, MI_SIZE) / 2 - 1;
  int y = row_offset * MI_SIZE + sign_r * AOMMAX(bh, MI_SIZE) / 2 - 1;

  pts[0] = GET_MV_SUBPEL(x);
  pts[1] = GET_MV_SUBPEL(y);
#if !CONFIG_COMPOUND_WARP_SAMPLES
  const int ref = 0;
#endif  // CONFIG_COMPOUND_WARP_SAMPLES
  pts_inref[0] = GET_MV_SUBPEL(x) + mbmi->mv[ref].as_mv.col;
  pts_inref[1] = GET_MV_SUBPEL(y) + mbmi->mv[ref].as_mv.row;
}

// Select samples according to the motion vector difference.
uint8_t av1_selectSamples(MV *mv, int *pts, int *pts_inref, int len,
                          BLOCK_SIZE bsize) {
  const int bw = block_size_wide[bsize];
  const int bh = block_size_high[bsize];
  const int thresh = clamp(AOMMAX(bw, bh), 16, 112);
  int pts_mvd[SAMPLES_ARRAY_SIZE] = { 0 };
  int i, j, k, l = len;
  uint8_t ret = 0;
  assert(len <= LEAST_SQUARES_SAMPLES_MAX);

  // Obtain the motion vector difference.
  for (i = 0; i < len; ++i) {
    pts_mvd[i] = abs(pts_inref[2 * i] - pts[2 * i] - mv->col) +
                 abs(pts_inref[2 * i + 1] - pts[2 * i + 1] - mv->row);

    if (pts_mvd[i] > thresh)
      pts_mvd[i] = -1;
    else
      ret++;
  }

  // Keep at least 1 sample.
  if (!ret) return 1;

  i = 0;
  j = l - 1;
  for (k = 0; k < l - ret; k++) {
    while (pts_mvd[i] != -1) i++;
    while (pts_mvd[j] == -1) j--;
    assert(i != j);
    if (i > j) break;

    // Replace the discarded samples;
    pts_mvd[i] = pts_mvd[j];
    pts[2 * i] = pts[2 * j];
    pts[2 * i + 1] = pts[2 * j + 1];
    pts_inref[2 * i] = pts_inref[2 * j];
    pts_inref[2 * i + 1] = pts_inref[2 * j + 1];
    i++;
    j--;
  }

  return ret;
}

// Note: Samples returned are at 1/8-pel precision
// Sample are the neighbor block center point's coordinates relative to the
// left-top pixel of current block.
uint8_t av1_findSamples(const AV1_COMMON *cm, MACROBLOCKD *xd, int *pts,
                        int *pts_inref) {
  const MB_MODE_INFO *const mbmi0 = xd->mi[0];
#if CONFIG_NEW_REF_SIGNALING
  const int ref_frame_nrs = mbmi0->ref_frame_nrs[0];
#else
  const int ref_frame = mbmi0->ref_frame[0];
#endif  // CONFIG_NEW_REF_SIGNALING
  const int up_available = xd->up_available;
  const int left_available = xd->left_available;
  int i, mi_step;
  uint8_t np = 0;
  int do_tl = 1;
  int do_tr = 1;
  const int mi_stride = xd->mi_stride;
  const int mi_row = xd->mi_row;
  const int mi_col = xd->mi_col;

  // scan the nearest above rows
  if (up_available) {
    const int mi_row_offset = -1;
    const MB_MODE_INFO *mbmi = xd->mi[mi_row_offset * mi_stride];
    uint8_t superblock_width = mi_size_wide[mbmi->sb_type];

    if (xd->width <= superblock_width) {
      // Handle "current block width <= above block width" case.
      const int col_offset = -mi_col % superblock_width;

      if (col_offset < 0) do_tl = 0;
      if (col_offset + superblock_width > xd->width) do_tr = 0;

#if CONFIG_COMPOUND_WARP_SAMPLES
      for (int ref = 0; ref < 1 + has_second_ref(mbmi); ++ref) {
#if CONFIG_NEW_REF_SIGNALING
        if (mbmi->ref_frame_nrs[ref] == ref_frame_nrs)
#else
        if (mbmi->ref_frame[ref] == ref_frame)
#endif  // CONFIG_NEW_REF_SIGNALING
        {
          record_samples(mbmi, ref, pts, pts_inref, 0, -1, col_offset, 1);
          pts += 2;
          pts_inref += 2;
          if (++np >= LEAST_SQUARES_SAMPLES_MAX) {
            return LEAST_SQUARES_SAMPLES_MAX;
          }
        }
      }
#else
#if CONFIG_NEW_REF_SIGNALING
      if (mbmi->ref_frame_nrs[0] == ref_frame_nrs &&
          mbmi->ref_frame_nrs[1] == INVALID_IDX)
#else
      if (mbmi->ref_frame[0] == ref_frame && mbmi->ref_frame[1] == NONE_FRAME)
#endif  // CONFIG_NEW_REF_SIGNALING
      {
        record_samples(mbmi, pts, pts_inref, 0, -1, col_offset, 1);
        pts += 2;
        pts_inref += 2;
        if (++np >= LEAST_SQUARES_SAMPLES_MAX) return LEAST_SQUARES_SAMPLES_MAX;
      }
#endif  // CONFIG_COMPOUND_WARP_SAMPLES
    } else {
      // Handle "current block width > above block width" case.
      for (i = 0; i < AOMMIN(xd->width, cm->mi_params.mi_cols - mi_col);
           i += mi_step) {
        mbmi = xd->mi[i + mi_row_offset * mi_stride];
        superblock_width = mi_size_wide[mbmi->sb_type];
        mi_step = AOMMIN(xd->width, superblock_width);
#if CONFIG_COMPOUND_WARP_SAMPLES
        for (int ref = 0; ref < 1 + has_second_ref(mbmi); ++ref) {
#if CONFIG_NEW_REF_SIGNALING
          if (mbmi->ref_frame_nrs[ref] == ref_frame_nrs)
#else
          if (mbmi->ref_frame[ref] == ref_frame)
#endif  // CONFIG_NEW_REF_SIGNALING
          {
            record_samples(mbmi, ref, pts, pts_inref, 0, -1, i, 1);
            pts += 2;
            pts_inref += 2;
            if (++np >= LEAST_SQUARES_SAMPLES_MAX)
              return LEAST_SQUARES_SAMPLES_MAX;
          }
        }
#else
#if CONFIG_NEW_REF_SIGNALING
        if (mbmi->ref_frame_nrs[0] == ref_frame_nrs &&
            mbmi->ref_frame_nrs[1] == INVALID_IDX)
#else
        if (mbmi->ref_frame[0] == ref_frame && mbmi->ref_frame[1] == NONE_FRAME)
#endif  // CONFIG_NEW_REF_SIGNALING
        {
          record_samples(mbmi, pts, pts_inref, 0, -1, i, 1);
          pts += 2;
          pts_inref += 2;
          if (++np >= LEAST_SQUARES_SAMPLES_MAX) {
            return LEAST_SQUARES_SAMPLES_MAX;
          }
        }
#endif  // CONFIG_COMPOUND_WARP_SAMPLES
      }
    }
  }
  assert(np <= LEAST_SQUARES_SAMPLES_MAX);

  // scan the nearest left columns
  if (left_available) {
    const int mi_col_offset = -1;
    const MB_MODE_INFO *mbmi = xd->mi[mi_col_offset];
    uint8_t superblock_height = mi_size_high[mbmi->sb_type];

    if (xd->height <= superblock_height) {
      // Handle "current block height <= above block height" case.
      const int row_offset = -mi_row % superblock_height;

      if (row_offset < 0) do_tl = 0;

#if CONFIG_COMPOUND_WARP_SAMPLES
      for (int ref = 0; ref < 1 + has_second_ref(mbmi); ++ref) {
#if CONFIG_NEW_REF_SIGNALING
        if (mbmi->ref_frame_nrs[ref] == ref_frame_nrs)
#else
        if (mbmi->ref_frame[ref] == ref_frame)
#endif  // CONFIG_NEW_REF_SIGNALING
        {
          record_samples(mbmi, ref, pts, pts_inref, row_offset, 1, 0, -1);
          pts += 2;
          pts_inref += 2;
          if (++np >= LEAST_SQUARES_SAMPLES_MAX) {
            return LEAST_SQUARES_SAMPLES_MAX;
          }
        }
      }
#else
#if CONFIG_NEW_REF_SIGNALING
      if (mbmi->ref_frame_nrs[0] == ref_frame_nrs &&
          mbmi->ref_frame_nrs[1] == INVALID_IDX)
#else
      if (mbmi->ref_frame[0] == ref_frame && mbmi->ref_frame[1] == NONE_FRAME)
#endif  // CONFIG_NEW_REF_SIGNALING
      {
        record_samples(mbmi, pts, pts_inref, row_offset, 1, 0, -1);
        pts += 2;
        pts_inref += 2;
        if (++np >= LEAST_SQUARES_SAMPLES_MAX) return LEAST_SQUARES_SAMPLES_MAX;
      }
#endif  // CONFIG_COMPOUND_WARP_SAMPLES
    } else {
      // Handle "current block height > above block height" case.
      for (i = 0; i < AOMMIN(xd->height, cm->mi_params.mi_rows - mi_row);
           i += mi_step) {
        mbmi = xd->mi[mi_col_offset + i * mi_stride];
        superblock_height = mi_size_high[mbmi->sb_type];
        mi_step = AOMMIN(xd->height, superblock_height);
#if CONFIG_COMPOUND_WARP_SAMPLES
        for (int ref = 0; ref < 1 + has_second_ref(mbmi); ++ref) {
#if CONFIG_NEW_REF_SIGNALING
          if (mbmi->ref_frame_nrs[ref] == ref_frame_nrs)
#else
          if (mbmi->ref_frame[ref] == ref_frame)
#endif  // CONFIG_NEW_REF_SIGNALING
          {
            record_samples(mbmi, ref, pts, pts_inref, i, 1, 0, -1);
            pts += 2;
            pts_inref += 2;
            if (++np >= LEAST_SQUARES_SAMPLES_MAX) {
              return LEAST_SQUARES_SAMPLES_MAX;
            }
          }
        }
#else
#if CONFIG_NEW_REF_SIGNALING
        if (mbmi->ref_frame_nrs[0] == ref_frame_nrs &&
            mbmi->ref_frame_nrs[1] == INVALID_IDX)
#else
        if (mbmi->ref_frame[0] == ref_frame && mbmi->ref_frame[1] == NONE_FRAME)
#endif  // CONFIG_NEW_REF_SIGNALING
        {
          record_samples(mbmi, pts, pts_inref, i, 1, 0, -1);
          pts += 2;
          pts_inref += 2;
          if (++np >= LEAST_SQUARES_SAMPLES_MAX) {
            return LEAST_SQUARES_SAMPLES_MAX;
          }
        }
#endif  // CONFIG_COMPOUND_WARP_SAMPLES
      }
    }
  }
  assert(np <= LEAST_SQUARES_SAMPLES_MAX);

  // Top-left block
  if (do_tl && left_available && up_available) {
    const int mi_row_offset = -1;
    const int mi_col_offset = -1;
    MB_MODE_INFO *mbmi = xd->mi[mi_col_offset + mi_row_offset * mi_stride];
#if CONFIG_COMPOUND_WARP_SAMPLES
    for (int ref = 0; ref < 1 + has_second_ref(mbmi); ++ref) {
#if CONFIG_NEW_REF_SIGNALING
      if (mbmi->ref_frame_nrs[ref] == ref_frame_nrs)
#else
      if (mbmi->ref_frame[ref] == ref_frame)
#endif  // CONFIG_NEW_REF_SIGNALING
      {
        record_samples(mbmi, ref, pts, pts_inref, 0, -1, 0, -1);
        pts += 2;
        pts_inref += 2;
        if (++np >= LEAST_SQUARES_SAMPLES_MAX) return LEAST_SQUARES_SAMPLES_MAX;
      }
    }
#else
#if CONFIG_NEW_REF_SIGNALING
    if (mbmi->ref_frame_nrs[0] == ref_frame_nrs &&
        mbmi->ref_frame_nrs[1] == INVALID_IDX)
#else
    if (mbmi->ref_frame[0] == ref_frame && mbmi->ref_frame[1] == NONE_FRAME)
#endif  // CONFIG_NEW_REF_SIGNALING
    {
      record_samples(mbmi, pts, pts_inref, 0, -1, 0, -1);
      pts += 2;
      pts_inref += 2;
      if (++np >= LEAST_SQUARES_SAMPLES_MAX) return LEAST_SQUARES_SAMPLES_MAX;
    }
#endif  // CONFIG_COMPOUND_WARP_SAMPLES
  }
  assert(np <= LEAST_SQUARES_SAMPLES_MAX);

  // Top-right block
  if (do_tr && has_top_right(cm, xd, mi_row, mi_col, xd->width)) {
    const POSITION trb_pos = { -1, xd->width };
    const TileInfo *const tile = &xd->tile;
    if (is_inside(tile, mi_col, mi_row, &trb_pos)) {
      const int mi_row_offset = -1;
      const int mi_col_offset = xd->width;
      const MB_MODE_INFO *mbmi =
          xd->mi[mi_col_offset + mi_row_offset * mi_stride];
#if CONFIG_COMPOUND_WARP_SAMPLES
      for (int ref = 0; ref < 1 + has_second_ref(mbmi); ++ref) {
#if CONFIG_NEW_REF_SIGNALING
        if (mbmi->ref_frame_nrs[ref] == ref_frame_nrs)
#else
        if (mbmi->ref_frame[ref] == ref_frame)
#endif  // CONFIG_NEW_REF_SIGNALING
        {
          record_samples(mbmi, ref, pts, pts_inref, 0, -1, xd->width, 1);
          pts += 2;
          pts_inref += 2;
          if (++np >= LEAST_SQUARES_SAMPLES_MAX) {
            return LEAST_SQUARES_SAMPLES_MAX;
          }
        }
      }
#else
#if CONFIG_NEW_REF_SIGNALING
      if (mbmi->ref_frame_nrs[0] == ref_frame_nrs &&
          mbmi->ref_frame_nrs[1] == INVALID_IDX)
#else
      if (mbmi->ref_frame[0] == ref_frame && mbmi->ref_frame[1] == NONE_FRAME)
#endif  // CONFIG_NEW_REF_SIGNALING
      {
        record_samples(mbmi, pts, pts_inref, 0, -1, xd->width, 1);
        if (++np >= LEAST_SQUARES_SAMPLES_MAX) return LEAST_SQUARES_SAMPLES_MAX;
      }
#endif  // CONFIG_COMPOUND_WARP_SAMPLES
    }
  }
  assert(np <= LEAST_SQUARES_SAMPLES_MAX);

  return np;
}

void av1_setup_skip_mode_allowed(AV1_COMMON *cm) {
  const OrderHintInfo *const order_hint_info = &cm->seq_params.order_hint_info;
  SkipModeInfo *const skip_mode_info = &cm->current_frame.skip_mode_info;

  skip_mode_info->skip_mode_allowed = 0;
  skip_mode_info->ref_frame_idx_0 = INVALID_IDX;
  skip_mode_info->ref_frame_idx_1 = INVALID_IDX;

  if (!order_hint_info->enable_order_hint || frame_is_intra_only(cm) ||
      cm->current_frame.reference_mode == SINGLE_REFERENCE)
    return;

  const int cur_order_hint = cm->current_frame.order_hint;
  int ref_order_hints[2] = { -1, INT_MAX };
  int ref_idx[2] = { INVALID_IDX, INVALID_IDX };

  // Identify the nearest forward and backward references.
#if CONFIG_NEW_REF_SIGNALING
  for (int i = 0; i < cm->new_ref_frame_data.n_total_refs; ++i) {
    const RefCntBuffer *const buf = get_ref_frame_buf_nrs(cm, i);
#else
  for (int i = 0; i < INTER_REFS_PER_FRAME; ++i) {
    const RefCntBuffer *const buf = get_ref_frame_buf(cm, LAST_FRAME + i);
#endif  // CONFIG_NEW_REF_SIGNALING
    if (buf == NULL) continue;

    // TODO(debargha, sarahparker): This could be implemented better based
    // on past and future lists, but this also works.
    const int ref_order_hint = buf->order_hint;
    if (get_relative_dist(order_hint_info, ref_order_hint, cur_order_hint) <
        0) {
      // Forward reference
      if (ref_order_hints[0] == -1 ||
          get_relative_dist(order_hint_info, ref_order_hint,
                            ref_order_hints[0]) > 0) {
        ref_order_hints[0] = ref_order_hint;
        ref_idx[0] = i;
      }
    } else if (get_relative_dist(order_hint_info, ref_order_hint,
                                 cur_order_hint) > 0) {
      // Backward reference
      if (ref_order_hints[1] == INT_MAX ||
          get_relative_dist(order_hint_info, ref_order_hint,
                            ref_order_hints[1]) < 0) {
        ref_order_hints[1] = ref_order_hint;
        ref_idx[1] = i;
      }
    }
  }

  if (ref_idx[0] != INVALID_IDX && ref_idx[1] != INVALID_IDX) {
    // == Bi-directional prediction ==
    skip_mode_info->skip_mode_allowed = 1;
#if CONFIG_NEW_REF_SIGNALING
    skip_mode_info->ref_frame_idx_0 = ref_idx[0];
    skip_mode_info->ref_frame_idx_1 = ref_idx[1];
#else
    skip_mode_info->ref_frame_idx_0 = AOMMIN(ref_idx[0], ref_idx[1]);
    skip_mode_info->ref_frame_idx_1 = AOMMAX(ref_idx[0], ref_idx[1]);
#endif  // CONFIG_NEW_REF_SIGNALING
  } else if (ref_idx[0] != INVALID_IDX && ref_idx[1] == INVALID_IDX) {
    // == Forward prediction only ==
    // Identify the second nearest forward reference.
    ref_order_hints[1] = -1;
#if CONFIG_NEW_REF_SIGNALING
    for (int i = 0; i < INTER_REFS_PER_FRAME_NRS; ++i) {
      const RefCntBuffer *const buf = get_ref_frame_buf_nrs(cm, i);
#else
    for (int i = 0; i < INTER_REFS_PER_FRAME; ++i) {
      const RefCntBuffer *const buf = get_ref_frame_buf(cm, LAST_FRAME + i);
#endif  // CONFIG_NEW_REF_SIGNALING
      if (buf == NULL) continue;

      const int ref_order_hint = buf->order_hint;
      if ((ref_order_hints[0] != -1 &&
           get_relative_dist(order_hint_info, ref_order_hint,
                             ref_order_hints[0]) < 0) &&
          (ref_order_hints[1] == -1 ||
           get_relative_dist(order_hint_info, ref_order_hint,
                             ref_order_hints[1]) > 0)) {
        // Second closest forward reference
        ref_order_hints[1] = ref_order_hint;
        ref_idx[1] = i;
      }
    }
    if (ref_order_hints[1] != -1) {
      skip_mode_info->skip_mode_allowed = 1;
#if CONFIG_NEW_REF_SIGNALING
      skip_mode_info->ref_frame_idx_0 = ref_idx[0];
      skip_mode_info->ref_frame_idx_1 = ref_idx[1];
#else
      skip_mode_info->ref_frame_idx_0 = AOMMIN(ref_idx[0], ref_idx[1]);
      skip_mode_info->ref_frame_idx_1 = AOMMAX(ref_idx[0], ref_idx[1]);
#endif  // CONFIG_NEW_REF_SIGNALING
    }
  }
}

#if !CONFIG_NEW_REF_SIGNALING
typedef struct {
  int map_idx;        // frame map index
  RefCntBuffer *buf;  // frame buffer
  int sort_idx;       // index based on the offset to be used for sorting
} REF_FRAME_INFO;

// Compares the sort_idx fields. If they are equal, then compares the map_idx
// fields to break the tie. This ensures a stable sort.
static int compare_ref_frame_info(const void *arg_a, const void *arg_b) {
  const REF_FRAME_INFO *info_a = (REF_FRAME_INFO *)arg_a;
  const REF_FRAME_INFO *info_b = (REF_FRAME_INFO *)arg_b;

  const int sort_idx_diff = info_a->sort_idx - info_b->sort_idx;
  if (sort_idx_diff != 0) return sort_idx_diff;
  return info_a->map_idx - info_b->map_idx;
}

static AOM_INLINE void set_ref_frame_info(int *remapped_ref_idx, int frame_idx,
                                          REF_FRAME_INFO *ref_info) {
  assert(frame_idx >= 0 && frame_idx < INTER_REFS_PER_FRAME);

  remapped_ref_idx[frame_idx] = ref_info->map_idx;
}

void av1_set_frame_refs(AV1_COMMON *const cm, int *remapped_ref_idx,
                        int lst_map_idx, int gld_map_idx) {
  int lst_frame_sort_idx = -1;
  int gld_frame_sort_idx = -1;

  assert(cm->seq_params.order_hint_info.enable_order_hint);
  assert(cm->seq_params.order_hint_info.order_hint_bits_minus_1 >= 0);
  const int cur_order_hint = (int)cm->current_frame.order_hint;
  const int cur_frame_sort_idx =
      1 << cm->seq_params.order_hint_info.order_hint_bits_minus_1;

  REF_FRAME_INFO ref_frame_info[REF_FRAMES];
  int ref_flag_list[INTER_REFS_PER_FRAME] = { 0, 0, 0, 0, 0, 0, 0 };

  for (int i = 0; i < REF_FRAMES; ++i) {
    const int map_idx = i;

    ref_frame_info[i].map_idx = map_idx;
    ref_frame_info[i].sort_idx = -1;

    RefCntBuffer *const buf = cm->ref_frame_map[map_idx];
    ref_frame_info[i].buf = buf;

    if (buf == NULL) continue;
    // If this assertion fails, there is a reference leak.
    assert(buf->ref_count > 0);

    const int offset = (int)buf->order_hint;
    ref_frame_info[i].sort_idx =
        (offset == -1) ? -1
                       : cur_frame_sort_idx +
                             get_relative_dist(&cm->seq_params.order_hint_info,
                                               offset, cur_order_hint);
    assert(ref_frame_info[i].sort_idx >= -1);

    if (map_idx == lst_map_idx) lst_frame_sort_idx = ref_frame_info[i].sort_idx;
    if (map_idx == gld_map_idx) gld_frame_sort_idx = ref_frame_info[i].sort_idx;
  }

  // Confirm both LAST_FRAME and GOLDEN_FRAME are valid forward reference
  // frames.
  if (lst_frame_sort_idx == -1 || lst_frame_sort_idx >= cur_frame_sort_idx) {
    aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                       "Inter frame requests a look-ahead frame as LAST");
  }
  if (gld_frame_sort_idx == -1 || gld_frame_sort_idx >= cur_frame_sort_idx) {
    aom_internal_error(&cm->error, AOM_CODEC_CORRUPT_FRAME,
                       "Inter frame requests a look-ahead frame as GOLDEN");
  }

  // Sort ref frames based on their frame_offset values.
  qsort(ref_frame_info, REF_FRAMES, sizeof(REF_FRAME_INFO),
        compare_ref_frame_info);

  // Identify forward and backward reference frames.
  // Forward  reference: offset < order_hint
  // Backward reference: offset >= order_hint
  int fwd_start_idx = 0, fwd_end_idx = REF_FRAMES - 1;

  for (int i = 0; i < REF_FRAMES; i++) {
    if (ref_frame_info[i].sort_idx == -1) {
      fwd_start_idx++;
      continue;
    }

    if (ref_frame_info[i].sort_idx >= cur_frame_sort_idx) {
      fwd_end_idx = i - 1;
      break;
    }
  }

  int bwd_start_idx = fwd_end_idx + 1;
  int bwd_end_idx = REF_FRAMES - 1;

  // === Backward Reference Frames ===

  // == ALTREF_FRAME ==
  if (bwd_start_idx <= bwd_end_idx) {
    set_ref_frame_info(remapped_ref_idx, ALTREF_FRAME - LAST_FRAME,
                       &ref_frame_info[bwd_end_idx]);
    ref_flag_list[ALTREF_FRAME - LAST_FRAME] = 1;
    bwd_end_idx--;
  }

  // == BWDREF_FRAME ==
  if (bwd_start_idx <= bwd_end_idx) {
    set_ref_frame_info(remapped_ref_idx, BWDREF_FRAME - LAST_FRAME,
                       &ref_frame_info[bwd_start_idx]);
    ref_flag_list[BWDREF_FRAME - LAST_FRAME] = 1;
    bwd_start_idx++;
  }

  // == ALTREF2_FRAME ==
  if (bwd_start_idx <= bwd_end_idx) {
    set_ref_frame_info(remapped_ref_idx, ALTREF2_FRAME - LAST_FRAME,
                       &ref_frame_info[bwd_start_idx]);
    ref_flag_list[ALTREF2_FRAME - LAST_FRAME] = 1;
  }

  // === Forward Reference Frames ===

  for (int i = fwd_start_idx; i <= fwd_end_idx; ++i) {
    // == LAST_FRAME ==
    if (ref_frame_info[i].map_idx == lst_map_idx) {
      set_ref_frame_info(remapped_ref_idx, LAST_FRAME - LAST_FRAME,
                         &ref_frame_info[i]);
      ref_flag_list[LAST_FRAME - LAST_FRAME] = 1;
    }

    // == GOLDEN_FRAME ==
    if (ref_frame_info[i].map_idx == gld_map_idx) {
      set_ref_frame_info(remapped_ref_idx, GOLDEN_FRAME - LAST_FRAME,
                         &ref_frame_info[i]);
      ref_flag_list[GOLDEN_FRAME - LAST_FRAME] = 1;
    }
  }

  assert(ref_flag_list[LAST_FRAME - LAST_FRAME] == 1 &&
         ref_flag_list[GOLDEN_FRAME - LAST_FRAME] == 1);

  // == LAST2_FRAME ==
  // == LAST3_FRAME ==
  // == BWDREF_FRAME ==
  // == ALTREF2_FRAME ==
  // == ALTREF_FRAME ==

  // Set up the reference frames in the anti-chronological order.
  static const MV_REFERENCE_FRAME ref_frame_list[INTER_REFS_PER_FRAME - 2] = {
    LAST2_FRAME, LAST3_FRAME, BWDREF_FRAME, ALTREF2_FRAME, ALTREF_FRAME
  };

  int ref_idx;
  for (ref_idx = 0; ref_idx < (INTER_REFS_PER_FRAME - 2); ref_idx++) {
    const MV_REFERENCE_FRAME ref_frame = ref_frame_list[ref_idx];

    if (ref_flag_list[ref_frame - LAST_FRAME] == 1) continue;

    while (fwd_start_idx <= fwd_end_idx &&
           (ref_frame_info[fwd_end_idx].map_idx == lst_map_idx ||
            ref_frame_info[fwd_end_idx].map_idx == gld_map_idx)) {
      fwd_end_idx--;
    }
    if (fwd_start_idx > fwd_end_idx) break;

    set_ref_frame_info(remapped_ref_idx, ref_frame - LAST_FRAME,
                       &ref_frame_info[fwd_end_idx]);
    ref_flag_list[ref_frame - LAST_FRAME] = 1;

    fwd_end_idx--;
  }

  // Assign all the remaining frame(s), if any, to the earliest reference
  // frame.
  for (; ref_idx < (INTER_REFS_PER_FRAME - 2); ref_idx++) {
    const MV_REFERENCE_FRAME ref_frame = ref_frame_list[ref_idx];
    if (ref_flag_list[ref_frame - LAST_FRAME] == 1) continue;
    set_ref_frame_info(remapped_ref_idx, ref_frame - LAST_FRAME,
                       &ref_frame_info[fwd_start_idx]);
    ref_flag_list[ref_frame - LAST_FRAME] = 1;
  }

  for (int i = 0; i < INTER_REFS_PER_FRAME; i++) {
    assert(ref_flag_list[i] == 1);
  }
}
#endif  // !CONFIG_NEW_REF_SIGNALING

#if CONFIG_REF_MV_BANK
static INLINE void update_ref_mv_bank(const MB_MODE_INFO *const mbmi,
                                      REF_MV_BANK *ref_mv_bank) {
  const MV_REFERENCE_FRAME ref_frame = av1_ref_frame_type(mbmi->ref_frame);
  CANDIDATE_MV *queue = ref_mv_bank->rmb_buffer[ref_frame];
  const int is_comp = has_second_ref(mbmi);
  const int start_idx = ref_mv_bank->rmb_start_idx[ref_frame];
  const int count = ref_mv_bank->rmb_count[ref_frame];
  int found = -1;

  // Check if current MV is already existing in the buffer.
  for (int i = 0; i < count; ++i) {
    const int idx = (start_idx + i) % REF_MV_BANK_SIZE;
    if (mbmi->mv[0].as_int == queue[idx].this_mv.as_int &&
        (!is_comp || mbmi->mv[1].as_int == queue[idx].comp_mv.as_int)) {
      found = i;
      break;
    }
  }

  // If current MV is found in the buffer, move it to the end of the buffer.
  if (found >= 0) {
    const int idx = (start_idx + found) % REF_MV_BANK_SIZE;
    const CANDIDATE_MV cand = queue[idx];
    for (int i = found; i < count - 1; ++i) {
      const int idx0 = (start_idx + i) % REF_MV_BANK_SIZE;
      const int idx1 = (start_idx + i + 1) % REF_MV_BANK_SIZE;
      queue[idx0] = queue[idx1];
    }
    const int tail = (start_idx + count - 1) % REF_MV_BANK_SIZE;
    queue[tail] = cand;
    return;
  }

  // If current MV is not found in the buffer, append it to the end of the
  // buffer, and update the count and start_idx accordingly.
  const int idx = (start_idx + count) % REF_MV_BANK_SIZE;
  queue[idx].this_mv = mbmi->mv[0];
  if (is_comp) queue[idx].comp_mv = mbmi->mv[1];
  if (count < REF_MV_BANK_SIZE) {
    ++ref_mv_bank->rmb_count[ref_frame];
  } else {
    ++ref_mv_bank->rmb_start_idx[ref_frame];
  }
}

void av1_update_ref_mv_bank(const AV1_COMMON *const cm, MACROBLOCKD *const xd,
                            const MB_MODE_INFO *const mbmi) {
  update_ref_mv_bank(mbmi, &xd->ref_mv_bank_left);
#if REF_MV_BANK_COLS
  const int col_bank_idx = av1_get_column_bank_index(cm, xd->mi_col);
  update_ref_mv_bank(mbmi, &xd->ref_mv_bank_above[col_bank_idx]);
#else
  (void)cm;
#endif  // REF_MV_BANK_COLS
}
#endif  // CONFIG_REF_MV_BANK
