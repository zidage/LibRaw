/* -*- C++ -*-
 * File: nikon_he_tile.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 * Copyright (C) 2026 Yurun Zi (code generated
 *   with assistance from GLM 5.2 and GPT 5.5)
 *
   Nikon HE per-tile decode orchestration implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE tile orchestrator — implementation.
//
// See nikon_he_tile.h for the public API.

#include "nikon_he_tile.h"
#include "nikon_he_precinct_decode.h"
#include <algorithm>
#include <cstring>

namespace nikon_he {

void TileWorkScratch::ensure(const LayoutInfo* li) {
    const int stripe_ints  = compute_buf_stripe_ints(li);
    const int passA_stride = 3 * li->lift_LB + li->memcpy_LB;
    const int lift_st      = li->lift_LB * 4;
    const int kBufInts     = 4 * passA_stride;
    constexpr int kMaxStripes = 40;

    if (static_cast<int>(bufA.size()) < kBufInts) bufA.assign(kBufInts, 0);
    if (static_cast<int>(bufB.size()) < kBufInts) bufB.assign(kBufInts, 0);
    if (static_cast<int>(h_work.size()) < 4 * lift_st + 16) {
        h_work.assign(4 * lift_st + 16, 0);
    }
    if (static_cast<int>(x2_carry.size()) < 4 * lift_st) {
        x2_carry.assign(4 * lift_st, 0);
    }
    if (static_cast<int>(x3_carry.size()) < 4 * lift_st) {
        x3_carry.assign(4 * lift_st, 0);
    }
    if (static_cast<int>(tile_buf.size()) < kMaxStripes * stripe_ints) {
        tile_buf.assign(kMaxStripes * stripe_ints, 0);
    }
}

void TileHorizStore::ensure(int store_precincts, int store_h_len) {
    h_len = store_h_len;
    precinct_count = store_precincts;
    const size_t need = static_cast<size_t>(store_precincts) * store_h_len;
    if (pass_a.size() < need) pass_a.assign(need, 0);
    if (pass_b.size() < need) pass_b.assign(need, 0);
}

static void run_one_ver_lift_loop(const int32_t* const x0_per_lb[4],
                                  int32_t* x1_base,
                                  int32_t* x2_carry_tile,
                                  int32_t* x3_carry_tile,
                                  VerLiftStatePerLB st[4],
                                  int lift_st) {
    const int lb_stride[4] = { lift_st, lift_st, 0, lift_st };

    int32_t* x1 = x1_base;
    for (int k = 0; k < 4; ++k) {
        const int n = lb_stride[k];
        st[k].x2_carry = x2_carry_tile + k * lift_st;
        st[k].x3_carry = x3_carry_tile + k * lift_st;
        ver_lift_lb_step(x0_per_lb[k], x1, n, st[k]);
        if (x1 != nullptr) x1 += n;
    }
}

bool decode_tile_entropy_horizontal(
    const uint8_t* const* precinct_data,
    const size_t* precinct_sizes,
    int image_width,
    const SubbandConfig config[26],
    PrecinctPredecessorState& pred_state,
    const uint8_t* predict_lut,
    TileWorkScratch& scratch,
    TileHorizStore& store,
    int precinct_count) {

    const LayoutInfo* li = config[0].layout_info;
    const int lift_st = li->lift_LB * 4;
    const int h_len = 4 * lift_st;

    scratch.ensure(li);
    store.ensure(precinct_count, h_len);

    for (int p = 0; p < precinct_count; ++p) {
        std::memset(scratch.bufA.data(), 0, scratch.bufA.size() * sizeof(int32_t));
        std::memset(scratch.bufB.data(), 0, scratch.bufB.size() * sizeof(int32_t));

        PrecinctDecodeResult prec = decode_precinct(
            precinct_data[p], precinct_sizes[p], image_width,
            config, pred_state, predict_lut,
            scratch.bufA.data(), scratch.bufB.data());
        if (!prec.success) return false;

        int32_t* ha = store.a(p);
        int32_t* hb = store.b(p);
        std::memset(ha, 0, static_cast<size_t>(h_len) * sizeof(int32_t));
        std::memset(hb, 0, static_cast<size_t>(h_len) * sizeof(int32_t));
        std::memset(scratch.h_work.data(), 0, scratch.h_work.size() * sizeof(int32_t));

        idwt_horizontal_pass(scratch.bufA.data(), config, /*is_passA=*/true,
                             ha, scratch.h_work.data());
        std::memset(scratch.h_work.data(), 0, scratch.h_work.size() * sizeof(int32_t));
        idwt_horizontal_pass(scratch.bufB.data(), config, /*is_passA=*/false,
                             hb, scratch.h_work.data());
    }
    return true;
}

void apply_tile_vertical_idwt(
    const SubbandConfig config[26],
    const TileHorizStore& store,
    int32_t* tile_coeff_buf,
    int tile_index,
    int32_t* overflow_carry,
    bool is_first_tile,
    bool is_last_tile,
    TileWorkScratch& scratch) {

    const LayoutInfo* li    = config[0].layout_info;
    const int stripe_ints   = compute_buf_stripe_ints(li);
    const int passA_stride  = 3 * li->lift_LB + li->memcpy_LB;
    const int lift_st       = li->lift_LB * 4;
    const int memcpy_st     = li->memcpy_LB * 4;
    const int kBand         = lift_st;
    (void)is_last_tile;

    scratch.ensure(li);
    std::fill(scratch.x2_carry.begin(), scratch.x2_carry.end(), 0);
    std::fill(scratch.x3_carry.begin(), scratch.x3_carry.end(), 0);
    std::fill(scratch.tile_buf.begin(), scratch.tile_buf.end(), 0);

    VerLiftStatePerLB ver_st[4];
    for (int k = 0; k < 4; ++k) {
        ver_st[k].state = is_first_tile ? VerLiftState::kInit2
                                        : VerLiftState::kInit0;
        ver_st[k].x2_carry = scratch.x2_carry.data() + k * lift_st;
        ver_st[k].x3_carry = scratch.x3_carry.data() + k * lift_st;
    }

    if (!is_first_tile) {
        std::memcpy(scratch.tile_buf.data(), overflow_carry,
                    static_cast<size_t>(2 * stripe_ints) * sizeof(int32_t));
    }

    int x1_write_offset = is_first_tile ? 0 : 8;
    int memcpy_cursor = 0;

    auto run_pass_verlift = [&](const int32_t* h) -> int {
        const int32_t* x0_lb0 = h + 0;
        const int32_t* x0_lb1 = h + lift_st;
        const int32_t* x0_lb3 = h + 2 * lift_st + memcpy_st;
        const int32_t* x0_per_lb[4] = { x0_lb0, x0_lb1, nullptr, x0_lb3 };

        const int pre_tick = ver_st[0].state;
        int32_t* x1_base = scratch.tile_buf.data() + x1_write_offset * passA_stride;
        run_one_ver_lift_loop(x0_per_lb, x1_base,
                              scratch.x2_carry.data(), scratch.x3_carry.data(),
                              ver_st, lift_st);
        return pre_tick;
    };

    for (int p = 0; p < store.precinct_count; ++p) {
        const int32_t* ha = store.a(p);
        std::memcpy(scratch.tile_buf.data() + memcpy_cursor + 3 * kBand,
                    ha + 2 * lift_st,
                    static_cast<size_t>(memcpy_st) * sizeof(int32_t));
        memcpy_cursor += stripe_ints;

        const bool skip_passA_verlift = (!is_first_tile && p == 0);
        if (!skip_passA_verlift) {
            const int pre_tick = run_pass_verlift(ha);
            if (pre_tick > 5) x1_write_offset += 4;
        }

        const int32_t* hb = store.b(p);
        std::memcpy(scratch.tile_buf.data() + memcpy_cursor + 3 * kBand,
                    hb + 2 * lift_st,
                    static_cast<size_t>(memcpy_st) * sizeof(int32_t));
        memcpy_cursor += stripe_ints;

        {
            const int pre_tick = run_pass_verlift(hb);
            if (pre_tick > 5) x1_write_offset += 4;
        }
    }

    const int32_t* tail_x0[4] = { nullptr, nullptr, nullptr, nullptr };
    for (int tail = 0; tail < 2; ++tail) {
        const int pre_tick = ver_st[0].state;
        int32_t* x1_base = scratch.tile_buf.data() + x1_write_offset * passA_stride;
        run_one_ver_lift_loop(tail_x0, x1_base,
                              scratch.x2_carry.data(), scratch.x3_carry.data(),
                              ver_st, lift_st);
        if (pre_tick > 5) x1_write_offset += 4;
    }

    int32_t* tile_out = tile_coeff_buf
                      + static_cast<size_t>(tile_index)
                          * kStripesPerTile * stripe_ints;
    std::memcpy(tile_out, scratch.tile_buf.data(),
                static_cast<size_t>(kStripesPerTile * stripe_ints)
                * sizeof(int32_t));

    std::memcpy(overflow_carry,
                scratch.tile_buf.data() + kStripesPerTile * stripe_ints,
                static_cast<size_t>(2 * stripe_ints) * sizeof(int32_t));
}

TileDecodeResult decode_tile(
    const uint8_t* const* precinct_data,
    const size_t* precinct_sizes,
    int image_width,
    const SubbandConfig config[26],
    PrecinctPredecessorState& pred_state,
    const uint8_t* predict_lut,
    int32_t* tile_coeff_buf,
    int tile_index,
    int32_t* overflow_carry,
    bool is_first_tile,
    bool is_last_tile,
    int precinct_count) {

    TileDecodeResult result = {0, false};
    TileWorkScratch scratch;
    TileHorizStore store;

    if (!decode_tile_entropy_horizontal(
            precinct_data, precinct_sizes, image_width, config,
            pred_state, predict_lut, scratch, store, precinct_count)) {
        return result;
    }

    apply_tile_vertical_idwt(config, store, tile_coeff_buf, tile_index,
                             overflow_carry, is_first_tile, is_last_tile,
                             scratch);
    result.precincts_decoded = precinct_count;
    result.success = true;
    return result;
}

}  // namespace nikon_he
