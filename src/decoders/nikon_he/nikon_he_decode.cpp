/* -*- C++ -*-
 * File: nikon_he_decode.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 * Copyright (C) 2026 Yurun Zi (code generated
 *   with assistance from GLM 5.2 and GPT 5.5)
 *
   Nikon HE image-level decoder implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE image-level decoder — implementation.
//
// See nikon_he_decode.h for the public API.

#include "nikon_he_decode.h"
#include "nikon_he_predecessor.h"
#include "nikon_he_precinct_decode.h"
#include "nikon_he_predict_lut.h"
#include "nikon_he_simd.h"

#include <atomic>
#include <vector>

namespace nikon_he {

static uint32_t read_be24(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 16)
         | (static_cast<uint32_t>(p[1]) << 8)
         | static_cast<uint32_t>(p[2]);
}

static int tile_working_rows(int image_height, int tile, int n_tiles) {
    const bool is_last = (tile == n_tiles - 1);
    if (is_last && (image_height % 64) != 0) {
        return (image_height % 64) / 2;
    }
    return kStripesPerTile;
}

HeDecodeResult decode_nikon_he_image(
    const uint8_t* precinct_data,
    size_t precinct_stream_size,
    int image_width,
    int image_height,
    const int32_t* lut,
    uint16_t* out_bayer) {

    HeDecodeResult result = {false, 0, 0};

    if (!precinct_data || precinct_stream_size < 68 || image_width <= 0 || image_height <= 0) {
        return result;
    }

    const int half_pass_W = image_width / 2;
    LayoutInfo layout_info;
    SubbandConfig config[26];
    compute_subband_layout(half_pass_W, config, layout_info);
    const LayoutInfo* li = config[0].layout_info;

    const uint8_t* predict_lut = build_prediction_lookup_table();

    const int n_tiles = (image_height + 63) / 64;
    const int n_file_precincts = n_tiles * 16 + 2;
    const int stripe_ints = compute_buf_stripe_ints(li);
    const int kband = compute_kband(li);

    std::vector<const uint8_t*> prec_ptrs(static_cast<size_t>(n_file_precincts), nullptr);
    std::vector<size_t> prec_sizes(static_cast<size_t>(n_file_precincts), 0);

    const uint8_t* cursor = precinct_data;
    size_t remaining = precinct_stream_size;
    int n_walked = 0;

    for (int p = 0; p < n_file_precincts; p++) {
        if (remaining < 3) break;
        const uint32_t sz = read_be24(cursor);
        if (sz == 0) break;
        const size_t full_size = static_cast<size_t>(sz) + 12;
        if (full_size > remaining) break;
        prec_ptrs[static_cast<size_t>(p)] = cursor;
        prec_sizes[static_cast<size_t>(p)] = full_size;
        cursor += full_size;
        remaining -= full_size;
        n_walked++;

        if ((p & 0xF) == 15 && remaining >= 6) {
            cursor += 6;
            remaining -= 6;
        }
    }
    (void)n_walked;

    const size_t coeff_count =
        static_cast<size_t>(n_tiles) * kStripesPerTile * stripe_ints;
    std::vector<int32_t> tile_coeff_buf(coeff_count, 0);
    std::vector<int32_t> step1_scratch(coeff_count, 0);
    std::vector<int32_t> overflow(static_cast<size_t>(2 * stripe_ints), 0);

    const int loop_n_tiles = n_tiles;
    std::atomic<int> tiles_decoded{0};
    std::atomic<int> total_precincts{0};
    std::atomic<bool> ok{true};

#if defined(_OPENMP)
#pragma omp parallel
#endif
    {
        TileWorkScratch scratch;
        TileHorizStore store;
        PrecinctPredecessorState pred_state;
        pred_state.init(config);

#if defined(_OPENMP)
#pragma omp for ordered schedule(dynamic, 1)
#endif
        for (int t = 0; t < loop_n_tiles; t++) {
            if (!ok.load(std::memory_order_relaxed)) {
#if defined(_OPENMP)
#pragma omp ordered
#endif
                { /* keep ordered sequence even on a failed tile */ }
                continue;
            }

            pred_state.reset_for_new_tile();
            const bool is_first = (t == 0);
            const bool is_last = (t == n_tiles - 1);
            const int tile_prec_base = t * 16;
            int prec_count = 0;
            for (int p = 0; p < kPrecinctsPerTile; ++p) {
                const int file_idx = tile_prec_base + p;
                if (file_idx >= n_file_precincts || !prec_ptrs[static_cast<size_t>(file_idx)]) {
                    break;
                }
                prec_count++;
            }

            const bool decoded = decode_tile_entropy_horizontal(
                prec_ptrs.data() + tile_prec_base,
                prec_sizes.data() + tile_prec_base,
                image_width, config, pred_state, predict_lut,
                scratch, store, prec_count);

#if defined(_OPENMP)
#pragma omp ordered
#endif
            {
                if (!decoded || !ok.load(std::memory_order_relaxed)) {
                    ok.store(false, std::memory_order_relaxed);
                } else {
                    apply_tile_vertical_idwt(
                        config, store, tile_coeff_buf.data(), t,
                        overflow.data(), is_first, is_last, scratch);
                    tiles_decoded.fetch_add(1, std::memory_order_relaxed);
                    total_precincts.fetch_add(prec_count, std::memory_order_relaxed);
                }
            }
        }
    }

    if (!ok.load(std::memory_order_relaxed)) {
        result.tiles_decoded = tiles_decoded.load();
        result.total_precincts = total_precincts.load();
        return result;
    }

#if defined(_OPENMP)
#pragma omp parallel for schedule(dynamic, 1)
#endif
    for (int t = 0; t < loop_n_tiles; t++) {
        const bool is_first = (t == 0);
        const bool is_last = (t == n_tiles - 1);
        const int w_rows = tile_working_rows(image_height, t, n_tiles);
        const int w_cols = image_width / 2;

        int32_t* tile_stripes = tile_coeff_buf.data()
            + static_cast<size_t>(t) * kStripesPerTile * stripe_ints;
        const int32_t* p1 = tile_stripes + kband;
        const int32_t* p2_orig = tile_stripes;
        const int32_t* p3 = tile_stripes + 3 * kband;
        const int32_t* p4 = tile_stripes + 2 * kband;

        int32_t* step1_L = step1_scratch.data()
            + static_cast<size_t>(t) * kStripesPerTile * stripe_ints;
        int32_t* step1_H = step1_L + kband;

        step1_merge_4_to_2(p1, p2_orig, p3, p4,
                           w_rows, w_cols,
                           stripe_ints, stripe_ints,
                           step1_L, step1_H,
                           stripe_ints,
                           is_first, is_last);
    }

#if defined(_OPENMP)
#pragma omp parallel for schedule(dynamic, 1)
#endif
    for (int t = 0; t < loop_n_tiles; t++) {
        const bool is_first = (t == 0);
        const bool is_last = (t == n_tiles - 1);
        const int w_rows = tile_working_rows(image_height, t, n_tiles);
        const int w_cols = image_width / 2;

        int32_t* tile_stripes = tile_coeff_buf.data()
            + static_cast<size_t>(t) * kStripesPerTile * stripe_ints;
        const int32_t* p1 = tile_stripes + kband;
        const int32_t* p4 = tile_stripes + 2 * kband;
        int32_t* step1_L = step1_scratch.data()
            + static_cast<size_t>(t) * kStripesPerTile * stripe_ints;
        int32_t* step1_H = step1_L + kband;
        const int tile_row_start = t * 64;

        step2_bayer_rows(p1, step1_L, step1_H, p4,
                         w_rows, w_cols,
                         stripe_ints, stripe_ints,
                         lut, out_bayer,
                         image_width, tile_row_start,
                         is_first, is_last);
    }

    result.success = true;
    result.tiles_decoded = tiles_decoded.load();
    result.total_precincts = total_precincts.load();
    return result;
}

}  // namespace nikon_he
