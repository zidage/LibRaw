/* -*- C++ -*-
 * File: nikon_he_gtli_table.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE GTLI (greatest truncatable level info) lookup table

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE GTLI lookup table — implementation.
//
// The table is indexed by (Bp, Br) → 26 sub-band GTLI values.
// Each row covers sub-bands 0..25 in this layout:
//   sb 0..5:   pass A wavelet horizontals, deepest 6 bands (LB 0)
//   sb 6..11:  pass A wavelet horizontals, next 6 bands (LB 1)
//   sb 12:     pass A LL band (LB 2)
//   sb 13..18: pass A wavelet horizontals, final 6 bands (LB 3)
//   sb 19..20: pass B wavelet horizontals, first 2 (LB 4)
//   sb 21..22: pass B wavelet horizontals, next 2 (LB 5)
//   sb 23:     pass B LL band (LB 6)
//   sb 24..25: pass B wavelet horizontals, final 2 (LB 7)

#include "nikon_he_gtli_table.h"

namespace nikon_he {

namespace {

// One (Bp, Br) table entry.
struct GtliRow {
    int Bp;
    int Br;
    uint8_t values[kSubBandsPerPrecinct];
};

// All known (Bp, Br) → GTLI mappings. Keep sorted by (Bp, Br) for
// readability; the lookup does a linear scan (table has only ~20 rows).
const GtliRow kGtliTable[] = {
    // Bp=4 rows
    {4, 0,  {1,1,2,2,3,3, 2,3,3,4,4,4, 4, 2,3,3,4,4,4, 3,4, 4,4, 4, 4,4}},
    {4, 1,  {1,1,2,2,3,3, 2,3,3,4,4,4, 4, 2,3,3,3,4,4, 3,4, 4,4, 4, 4,4}},
    {4, 2,  {1,1,2,2,3,3, 2,3,3,3,4,4, 4, 2,3,3,3,4,4, 3,4, 4,4, 4, 4,4}},
    {4, 3,  {0,1,2,2,3,3, 2,3,3,3,4,4, 4, 2,3,3,3,4,4, 3,4, 4,4, 4, 4,4}},
    {4, 4,  {0,1,2,2,3,3, 2,2,3,3,4,4, 4, 2,3,3,3,4,4, 3,4, 4,4, 4, 4,4}},
    {4, 5,  {0,1,2,2,3,3, 2,2,3,3,4,4, 4, 2,2,3,3,4,4, 3,4, 4,4, 4, 4,4}},
    {4, 6,  {0,1,2,2,3,3, 1,2,3,3,4,4, 4, 2,2,3,3,4,4, 3,4, 4,4, 4, 4,4}},
    {4, 7,  {0,1,2,2,3,3, 1,2,3,3,4,4, 4, 1,2,3,3,4,4, 3,4, 4,4, 4, 4,4}},
    {4, 8,  {0,1,2,2,3,3, 1,2,3,3,3,4, 4, 1,2,3,3,4,4, 3,4, 4,4, 4, 4,4}},
    {4, 9,  {0,1,2,2,3,3, 1,2,3,3,3,4, 4, 1,2,3,3,3,4, 3,4, 4,4, 4, 4,4}},
    {4,10,  {0,1,1,2,3,3, 1,2,3,3,3,4, 4, 1,2,3,3,3,4, 3,4, 4,4, 4, 4,4}},
    {4,11,  {0,1,1,2,2,3, 1,2,3,3,3,4, 4, 1,2,3,3,3,4, 3,4, 4,4, 4, 4,4}},
    {4,12,  {0,1,1,2,2,3, 1,2,3,3,3,4, 3, 1,2,3,3,3,4, 3,4, 4,4, 3, 4,4}},
    // Bp=5 rows
    {5,12,  {1,2,2,3,3,4, 2,3,4,4,4,5, 4, 2,3,4,4,4,5, 4,5, 5,5, 4, 5,5}},
    {5,13,  {1,2,2,3,3,4, 2,3,4,4,4,5, 4, 2,3,4,4,4,5, 4,5, 5,5, 4, 4,5}},
    {5,14,  {1,2,2,3,3,4, 2,3,4,4,4,4, 4, 2,3,4,4,4,5, 4,5, 5,5, 4, 4,5}},
    {5,15,  {1,2,2,3,3,4, 2,3,4,4,4,4, 4, 2,3,4,4,4,4, 4,5, 5,5, 4, 4,5}},
    {5,16,  {1,2,2,3,3,4, 2,3,4,4,4,4, 4, 2,3,4,4,4,4, 4,5, 4,5, 4, 4,5}},
    {5,17,  {1,2,2,3,3,4, 2,3,4,4,4,4, 4, 2,3,3,4,4,4, 4,5, 4,5, 4, 4,5}},
    {5,18,  {1,2,2,3,3,4, 2,3,3,4,4,4, 4, 2,3,3,4,4,4, 4,5, 4,5, 4, 4,5}},
    {5,19,  {1,2,2,3,3,4, 2,3,3,4,4,4, 4, 2,3,3,4,4,4, 4,4, 4,5, 4, 4,5}},
    {5,20,  {1,1,2,3,3,4, 2,3,3,4,4,4, 4, 2,3,3,4,4,4, 4,4, 4,5, 4, 4,5}},
    {5,21,  {1,1,2,3,3,4, 2,3,3,4,4,4, 4, 2,3,3,4,4,4, 3,4, 4,5, 4, 4,5}},
    {5,22,  {1,1,2,3,3,3, 2,3,3,4,4,4, 4, 2,3,3,4,4,4, 3,4, 4,5, 4, 4,5}},
    {5,23,  {1,1,2,2,3,3, 2,3,3,4,4,4, 4, 2,3,3,4,4,4, 3,4, 4,5, 4, 4,5}},
    {5,24,  {1,1,2,2,3,3, 2,3,3,4,4,4, 4, 2,3,3,4,4,4, 3,4, 4,4, 4, 4,5}},

    // HE* (Lossy_High_Efficiency_Star) rows. Captured 2026-05-22 from
    // raw.pixls.us "Lossy_High_Efficiency_Star" sample. 38 unique
    // (Bp, Br) combos, all gtli-consistent across precincts.
    {1,  0, {0,0,0,0,0,0, 0,0,0,1,1,1, 1, 0,0,0,1,1,1, 0,1, 1,1, 1, 1,1}},
    {1,  1, {0,0,0,0,0,0, 0,0,0,1,1,1, 1, 0,0,0,0,1,1, 0,1, 1,1, 1, 1,1}},
    {1,  7, {0,0,0,0,0,0, 0,0,0,0,1,1, 1, 0,0,0,0,1,1, 0,1, 1,1, 1, 1,1}},
    {1,  8, {0,0,0,0,0,0, 0,0,0,0,0,1, 1, 0,0,0,0,1,1, 0,1, 1,1, 1, 1,1}},
    {1, 11, {0,0,0,0,0,0, 0,0,0,0,0,1, 1, 0,0,0,0,0,1, 0,1, 1,1, 1, 1,1}},
    {2,  0, {0,0,0,0,1,1, 0,1,1,2,2,2, 2, 0,1,1,2,2,2, 1,2, 2,2, 2, 2,2}},
    {2,  1, {0,0,0,0,1,1, 0,1,1,2,2,2, 2, 0,1,1,1,2,2, 1,2, 2,2, 2, 2,2}},
    {2,  3, {0,0,0,0,1,1, 0,1,1,1,2,2, 2, 0,1,1,1,2,2, 1,2, 2,2, 2, 2,2}},
    {2,  4, {0,0,0,0,1,1, 0,0,1,1,2,2, 2, 0,1,1,1,2,2, 1,2, 2,2, 2, 2,2}},
    {2,  7, {0,0,0,0,1,1, 0,0,1,1,2,2, 2, 0,0,1,1,2,2, 1,2, 2,2, 2, 2,2}},
    {2,  8, {0,0,0,0,1,1, 0,0,1,1,1,2, 2, 0,0,1,1,2,2, 1,2, 2,2, 2, 2,2}},
    {2, 10, {0,0,0,0,1,1, 0,0,1,1,1,2, 2, 0,0,1,1,1,2, 1,2, 2,2, 2, 2,2}},
    {2, 11, {0,0,0,0,0,1, 0,0,1,1,1,2, 2, 0,0,1,1,1,2, 1,2, 2,2, 2, 2,2}},
    {2, 12, {0,0,0,0,0,1, 0,0,1,1,1,2, 1, 0,0,1,1,1,2, 1,2, 2,2, 1, 2,2}},
    {2, 13, {0,0,0,0,0,1, 0,0,1,1,1,2, 1, 0,0,1,1,1,2, 1,2, 2,2, 1, 1,2}},
    {2, 14, {0,0,0,0,0,1, 0,0,1,1,1,1, 1, 0,0,1,1,1,2, 1,2, 2,2, 1, 1,2}},
    {2, 15, {0,0,0,0,0,1, 0,0,1,1,1,1, 1, 0,0,1,1,1,1, 1,2, 2,2, 1, 1,2}},
    {2, 16, {0,0,0,0,0,1, 0,0,1,1,1,1, 1, 0,0,1,1,1,1, 1,2, 1,2, 1, 1,2}},
    {2, 17, {0,0,0,0,0,1, 0,0,1,1,1,1, 1, 0,0,0,1,1,1, 1,2, 1,2, 1, 1,2}},
    {2, 18, {0,0,0,0,0,1, 0,0,0,1,1,1, 1, 0,0,0,1,1,1, 1,2, 1,2, 1, 1,2}},
    {2, 20, {0,0,0,0,0,1, 0,0,0,1,1,1, 1, 0,0,0,1,1,1, 1,1, 1,2, 1, 1,2}},
    {2, 21, {0,0,0,0,0,1, 0,0,0,1,1,1, 1, 0,0,0,1,1,1, 0,1, 1,2, 1, 1,2}},
    {2, 23, {0,0,0,0,0,0, 0,0,0,1,1,1, 1, 0,0,0,1,1,1, 0,1, 1,2, 1, 1,2}},
    {2, 24, {0,0,0,0,0,0, 0,0,0,1,1,1, 1, 0,0,0,1,1,1, 0,1, 1,1, 1, 1,2}},
    {3,  8, {0,0,1,1,2,2, 0,1,2,2,2,3, 3, 0,1,2,2,3,3, 2,3, 3,3, 3, 3,3}},
    {3,  9, {0,0,1,1,2,2, 0,1,2,2,2,3, 3, 0,1,2,2,2,3, 2,3, 3,3, 3, 3,3}},
    {3, 10, {0,0,0,1,2,2, 0,1,2,2,2,3, 3, 0,1,2,2,2,3, 2,3, 3,3, 3, 3,3}},
    {3, 11, {0,0,0,1,1,2, 0,1,2,2,2,3, 3, 0,1,2,2,2,3, 2,3, 3,3, 3, 3,3}},
    {3, 12, {0,0,0,1,1,2, 0,1,2,2,2,3, 2, 0,1,2,2,2,3, 2,3, 3,3, 2, 3,3}},
    {3, 13, {0,0,0,1,1,2, 0,1,2,2,2,3, 2, 0,1,2,2,2,3, 2,3, 3,3, 2, 2,3}},
    {3, 14, {0,0,0,1,1,2, 0,1,2,2,2,2, 2, 0,1,2,2,2,3, 2,3, 3,3, 2, 2,3}},
    {3, 15, {0,0,0,1,1,2, 0,1,2,2,2,2, 2, 0,1,2,2,2,2, 2,3, 3,3, 2, 2,3}},
    {3, 16, {0,0,0,1,1,2, 0,1,2,2,2,2, 2, 0,1,2,2,2,2, 2,3, 2,3, 2, 2,3}},
    {3, 17, {0,0,0,1,1,2, 0,1,2,2,2,2, 2, 0,1,1,2,2,2, 2,3, 2,3, 2, 2,3}},
    {3, 18, {0,0,0,1,1,2, 0,1,1,2,2,2, 2, 0,1,1,2,2,2, 2,3, 2,3, 2, 2,3}},
    {3, 20, {0,0,0,1,1,2, 0,1,1,2,2,2, 2, 0,1,1,2,2,2, 2,2, 2,3, 2, 2,3}},
    {3, 21, {0,0,0,1,1,2, 0,1,1,2,2,2, 2, 0,1,1,2,2,2, 1,2, 2,3, 2, 2,3}},
    {3, 22, {0,0,0,1,1,1, 0,1,1,2,2,2, 2, 0,1,1,2,2,2, 1,2, 2,3, 2, 2,3}},
    {3, 23, {0,0,0,0,1,1, 0,1,1,2,2,2, 2, 0,1,1,2,2,2, 1,2, 2,3, 2, 2,3}},
    {3, 24, {0,0,0,0,1,1, 0,1,1,2,2,2, 2, 0,1,1,2,2,2, 1,2, 2,2, 2, 2,3}},
};

constexpr int kNumGtliRows = sizeof(kGtliTable) / sizeof(kGtliTable[0]);

}  // namespace

const uint8_t* lookup_gtli_table(int Bp, int Br) {
    // 1. Exact (Bp, Br) match.
    for (int i = 0; i < kNumGtliRows; ++i) {
        if (kGtliTable[i].Bp == Bp && kGtliTable[i].Br == Br) {
            return kGtliTable[i].values;
        }
    }

    // 2. Fallback: derive from a higher-Bp row at the same Br.
    //
    // GTLI decreases by exactly 1 per Bp level (one fewer bit of precision
    // per lower Bp), clamped at 0. This "downward" derivation
    // (subtract the Bp gap, clamp) is exact for the overlapping rows seen in
    // Z8/Z9 HE and HE* samples, so it is preferred whenever a higher-Bp row
    // exists.
    //
    // Pick the closest higher Bp available at this Br to keep the source
    // row's clamping as far from the target as possible.
    int best_src = -1;
    int best_gap = 0;
    for (int i = 0; i < kNumGtliRows; ++i) {
        if (kGtliTable[i].Br != Br) continue;
        int gap = kGtliTable[i].Bp - Bp;
        if (gap <= 0) continue;
        if (best_src < 0 || gap < best_gap) {
            best_src = i;
            best_gap = gap;
        }
    }
    if (best_src >= 0) {
        static uint8_t derived[kSubBandsPerPrecinct];
        for (int s = 0; s < kSubBandsPerPrecinct; ++s) {
            int v = kGtliTable[best_src].values[s] - best_gap;
            derived[s] = (v < 0) ? 0 : static_cast<uint8_t>(v);
        }
        return derived;
    }

    // 3. Higher-Bp extrapolation: Z6_3 HE shows that Bp/Br regimes are
    // selected dynamically by the encoder, not fixed by camera model or
    // menu label. It uses Bp=6 and low-Br Bp=5 rows not present in the
    // original Z8/Z9 table. With no higher-Bp row available, use the closest
    // lower-Bp row at the same Br and add one GTLI level per Bp step. This is
    // the inverse of the downward rule and is intentionally lower priority
    // than exact/downward lookup because lower-Bp rows may have lost
    // pre-clamp detail.
    best_src = -1;
    best_gap = 0;
    for (int i = 0; i < kNumGtliRows; ++i) {
        if (kGtliTable[i].Br != Br) continue;
        int gap = Bp - kGtliTable[i].Bp;
        if (gap <= 0) continue;
        if (best_src < 0 || gap < best_gap) {
            best_src = i;
            best_gap = gap;
        }
    }
    if (best_src >= 0) {
        static uint8_t derived[kSubBandsPerPrecinct];
        for (int s = 0; s < kSubBandsPerPrecinct; ++s) {
            derived[s] = static_cast<uint8_t>(kGtliTable[best_src].values[s] + best_gap);
        }
        return derived;
    }

    return nullptr;
}

}  // namespace nikon_he
