/* -*- C++ -*-
 * File: nikon_he_idwt_horizontal.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE horizontal inverse DWT implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE horizontal 5/3 IDWT — implementation.
//
// See nikon_he_idwt_horizontal.h for the public API.

#include "nikon_he_idwt_horizontal.h"
#include "nikon_he_simd.h"
#include <cstring>

namespace nikon_he {

static inline int round_up(int x, int m) {
    return ((x + m - 1) / m) * m;
}

void idwt53_inverse_one_level(
    const int32_t* NIKON_HE_RESTRICT L, int n_L,
    const int32_t* NIKON_HE_RESTRICT H, int n_H,
    int32_t* NIKON_HE_RESTRICT out) {

    const int n = n_L + n_H;
    if (n == 0) return;
    (void)n_L;
    (void)n_H;

    // n_L = ceil(n/2), n_H = floor(n/2) for a 5/3 level.
    const int n_pairs = n >> 1;
    NIKON_HE_SIMD_LOOP
    for (int i = 0; i < n_pairs; ++i) {
        out[2 * i]     = L[i];
        out[2 * i + 1] = H[i];
    }
    if (n & 1) {
        out[n - 1] = L[n_pairs];
    }

    // Inverse UPDATE on even samples. Interior evens only read odd
    // neighbors, so the even writes have no loop-carried dependence.
    if (n == 1) {
        out[0] -= (0 + 0 + 2) >> 2;
    } else {
        out[0] -= (out[1] + out[1] + 2) >> 2;
        const int last_even = (n - 1) & ~1;
        NIKON_HE_SIMD_LOOP
        for (int i = 2; i < last_even; i += 2) {
            out[i] -= (out[i - 1] + out[i + 1] + 2) >> 2;
        }
        if (last_even >= 2) {
            if (last_even + 1 < n) {
                out[last_even] -= (out[last_even - 1] + out[last_even + 1] + 2) >> 2;
            } else {
                out[last_even] -= (out[last_even - 1] + out[last_even - 1] + 2) >> 2;
            }
        }
    }

    // Inverse PREDICT on odd samples. Odds only read the updated evens.
    if (n >= 2) {
        const int last_odd = (n & 1) ? (n - 2) : (n - 1);
        NIKON_HE_SIMD_LOOP
        for (int i = 1; i < last_odd; i += 2) {
            out[i] += (out[i - 1] + out[i + 1]) >> 1;
        }
        if (last_odd >= 1) {
            if (last_odd + 1 < n) {
                out[last_odd] += (out[last_odd - 1] + out[last_odd + 1]) >> 1;
            } else {
                out[last_odd] += (out[last_odd - 1] + out[last_odd - 1]) >> 1;
            }
        }
    }
}

void idwt53_horizontal_lift_all(
    int32_t* in_buf,
    int n,
    int levels,
    const int* hl_offsets,
    int32_t* out,
    int32_t* work) {

    if (levels < 1 || n < 1) return;

    int N[8];
    N[0] = n;
    for (int k = 1; k <= levels; ++k) {
        N[k] = (N[k - 1] + 1) / 2;
    }

    int32_t* buf_a = out;
    int32_t* buf_b = work;
    int32_t* cur = (levels & 1) ? buf_b : buf_a;
    int32_t* dst = (cur == buf_a) ? buf_b : buf_a;

    const int ll_size = N[levels];
    NIKON_HE_SIMD_LOOP
    for (int i = 0; i < ll_size; ++i) {
        cur[i] = in_buf[i];
    }

    for (int k = levels; k >= 1; --k) {
        const int n_L = N[k];
        const int n_H = N[k - 1] - N[k];
        const int32_t* H = in_buf + hl_offsets[k];
        idwt53_inverse_one_level(cur, n_L, H, n_H, dst);
        int32_t* tmp = cur;
        cur = dst;
        dst = tmp;
    }
}

void idwt_horizontal_pass(
    const int32_t* buf,
    const SubbandConfig config[26],
    bool is_passA,
    int32_t* out_stripe,
    int32_t* work) {

    const LayoutInfo* li = config[0].layout_info;
    const int lift_LB    = li->lift_LB;
    const int memcpy_LB  = li->memcpy_LB;
    const int n          = li->ng_LL * 4;
    const int lift_st    = lift_LB * 4;

    if (is_passA) {
        const int passA_levels = 5;
        int hl_offsets[6];
        int acc = 0;
        int cum[6];
        for (int i = 0; i < 6; ++i) {
            acc += round_up(li->ng_lift[i], 8);
            cum[i] = acc;
        }
        for (int k = 0; k <= 5; ++k) {
            hl_offsets[k] = 4 * cum[5 - k];
        }

        const int lb0_out = 0;
        const int lb1_out = lift_st;
        const int lb2_out = 2 * lift_st;
        const int lb3_out = 2 * lift_st + memcpy_LB * 4;

        int32_t* lb0_in = const_cast<int32_t*>(buf) + 0;
        int32_t* lb1_in = const_cast<int32_t*>(buf) + lift_st;
        int32_t* lb3_in = const_cast<int32_t*>(buf) + 2 * lift_st + memcpy_LB * 4;

        idwt53_horizontal_lift_all(lb0_in, n, passA_levels,
                                   hl_offsets,
                                   out_stripe + lb0_out, work);
        idwt53_horizontal_lift_all(lb1_in, n, passA_levels,
                                   hl_offsets,
                                   out_stripe + lb1_out, work);

        std::memcpy(out_stripe + lb2_out,
                    buf + config[12].x24 * 4,
                    static_cast<size_t>(config[12].ng * 4)
                    * sizeof(int32_t));

        idwt53_horizontal_lift_all(lb3_in, n, passA_levels,
                                   hl_offsets,
                                   out_stripe + lb3_out, work);
    } else {
        const int passB_levels = 1;
        const int ng_passB[2] = { li->ng_max, li->ng_max };
        int hl_offsets[2];
        int acc = 0;
        int cum[2];
        for (int i = 0; i < 2; ++i) {
            acc += round_up(ng_passB[i], 8);
            cum[i] = acc;
        }
        for (int k = 0; k <= 1; ++k) {
            hl_offsets[k] = 4 * cum[1 - k];
        }

        const int lb4_out = 0;
        const int lb5_out = lift_st;
        const int lb6_out = 2 * lift_st;
        const int lb7_out = 2 * lift_st + memcpy_LB * 4;

        int32_t* lb4_in = const_cast<int32_t*>(buf) + 0;
        int32_t* lb5_in = const_cast<int32_t*>(buf) + lift_st;
        int32_t* lb7_in = const_cast<int32_t*>(buf) + 2 * lift_st + memcpy_LB * 4;

        idwt53_horizontal_lift_all(lb4_in, n, passB_levels,
                                   hl_offsets,
                                   out_stripe + lb4_out, work);
        idwt53_horizontal_lift_all(lb5_in, n, passB_levels,
                                   hl_offsets,
                                   out_stripe + lb5_out, work);

        std::memcpy(out_stripe + lb6_out,
                    buf + config[23].x24 * 4,
                    static_cast<size_t>(config[23].ng * 4)
                    * sizeof(int32_t));

        idwt53_horizontal_lift_all(lb7_in, n, passB_levels,
                                   hl_offsets,
                                   out_stripe + lb7_out, work);
    }
}

}  // namespace nikon_he
