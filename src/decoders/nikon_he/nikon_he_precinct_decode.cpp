/* -*- C++ -*-
 * File: nikon_he_precinct_decode.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE per-precinct entropy decode implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE per-precinct entropy decode — implementation.
//
// See nikon_he_precinct_decode.h for the public API.

#include "nikon_he_precinct_decode.h"
#include "nikon_he_gcli_decode.h"
#include "nikon_he_coefficient_decode.h"
#include "nikon_he_gtli_table.h"
#include "nikon_he_predict_lut.h"
#include <algorithm>

namespace nikon_he {

static const int kLbSbRange[8][2] = {
    {0, 6}, {6, 12}, {12, 13}, {13, 19},
    {19, 21}, {21, 23}, {23, 24}, {24, 26},
};

void compute_subband_byte_slice(
    int lb_sig_remaining,
    int lb_gcli_remaining,
    int lb_data_remaining,
    int lb_sign_remaining,
    int total_ng_in_lb,
    int sb_ng,
    int& out_sig_bytes,
    int& out_gcli_bytes,
    int& out_data_bytes,
    int& out_sign_bytes) {

    if (total_ng_in_lb <= sb_ng) {
        out_sig_bytes  = lb_sig_remaining;
        out_gcli_bytes = lb_gcli_remaining;
        out_data_bytes = lb_data_remaining;
        out_sign_bytes = lb_sign_remaining;
        return;
    }

    out_sig_bytes  = (lb_sig_remaining * sb_ng) / total_ng_in_lb;
    out_gcli_bytes = (lb_gcli_remaining * sb_ng) / total_ng_in_lb;
    out_data_bytes = (lb_data_remaining * sb_ng) / total_ng_in_lb;
    out_sign_bytes = (lb_sign_remaining * sb_ng) / total_ng_in_lb;

    const int min_sig = (sb_ng + 7) / 8;
    if (out_sig_bytes < min_sig) {
        out_sig_bytes = std::min(min_sig, lb_sig_remaining);
    }
}

PrecinctDecodeResult decode_precinct(
    const uint8_t* precinct_data,
    size_t precinct_size,
    int image_width,
    const SubbandConfig config[26],
    PrecinctPredecessorState& pred_state,
    const uint8_t* predict_lut,
    int32_t* bufA,
    int32_t* bufB) {

    PrecinctDecodeResult result = {0, false};

    PrecinctSizes sizes;
    const int* lb_sig = (config[0].layout_info)
                        ? config[0].layout_info->lb_sig_bytes
                        : nullptr;
    if (!parse_precinct_header(precinct_data, precinct_size, image_width,
                               sizes, lb_sig)) {
        return result;
    }

    if (should_reset_gcli(pred_state.precinct_index(), sizes.Bp, sizes.Br)) {
        pred_state.reset_gcli_state(config);
    }

    const uint8_t* gtli_row = lookup_gtli_table(sizes.Bp, sizes.Br);

    for (int lb = 0; lb < 8; ++lb) {
        const uint8_t* sig_buf  = precinct_data + sizes.lb_sig_offset[lb];
        const uint8_t* gcli_buf = sig_buf  + sizes.lb_sig_bytes[lb];
        const uint8_t* data_buf = gcli_buf + sizes.lb_gcli_bytes[lb];
        const uint8_t* sign_buf = data_buf + sizes.lb_data_bytes[lb];

        BitReader sig_reader (sig_buf,  sizes.lb_sig_bytes[lb]);
        BitReader gcli_reader(gcli_buf, sizes.lb_gcli_bytes[lb]);
        BitReader data_reader(data_buf, sizes.lb_data_bytes[lb]);
        BitReader sign_reader(sign_buf, sizes.lb_sign_bytes[lb]);

        const int sb_start = kLbSbRange[lb][0];
        const int sb_end   = kLbSbRange[lb][1];

        for (int sb = sb_start; sb < sb_end; ++sb) {
            const int ng = config[sb].ng;
            const int dpb_mode = (sb >= 0 && sb < 28)
                                 ? (sizes.Dpb[sb] | 0x70)
                                 : 0x71;

            int gtli = gtli_row ? static_cast<int>(gtli_row[sb]) : 0;
            if (gtli == 0xFF) gtli = 0;

            const uint8_t* prev_gcli = pred_state.get_previous_gcli(sb);
            uint8_t* gcli_out = pred_state.gcli_buffer(sb);
            if (!gcli_out && ng > 0) {
                return result;
            }

            decode_gcli_values(sig_reader, gcli_reader, dpb_mode,
                               ng, gtli, predict_lut,
                               prev_gcli, gcli_out);

            int32_t* target_buf = (config[sb].buffer_idx == 0) ? bufA : bufB;
            int32_t* coeffs = target_buf + config[sb].x24 * 4;
            const bool all_zero = unpack_sign_and_dequantize(
                data_reader, sign_reader, gcli_out, gtli, ng, coeffs);
            pred_state.set_fully_insig(sb, all_zero);

            result.total_sub_bands_decoded++;
        }
    }

    pred_state.advance_precinct();

    result.success = (result.total_sub_bands_decoded == 26);
    return result;
}

}  // namespace nikon_he
