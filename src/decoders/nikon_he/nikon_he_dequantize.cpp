/* -*- C++ -*-
 * File: nikon_he_dequantize.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE coefficient dequantization implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE dequantization — implementation.
//
// See nikon_he_dequantize.h for the public API.

#include "nikon_he_dequantize.h"
#include "nikon_he_simd.h"

namespace nikon_he {

int32_t dequantize_coefficient(int32_t coefficient,
                               int gcli,
                               int gtli) {
    if (coefficient == 0) {
        return 0;
    }
    if (gcli <= gtli) {
        return 0;
    }
    const int bit_plane_count = gcli - gtli;
    if (bit_plane_count < 1 || bit_plane_count > 15) {
        return 0;
    }
    const uint32_t magnitude =
        static_cast<uint32_t>(coefficient < 0 ? -coefficient : coefficient) >> gtli;
    const uint64_t product = static_cast<uint64_t>(magnitude)
                           * static_cast<uint64_t>(kMidpointScaleTable[bit_plane_count - 1]);
    const int32_t shifted = static_cast<int32_t>(product >> (16 - gtli));
    const int32_t result  = shifted << kW4Shift;
    return (coefficient < 0) ? -result : result;
}

int32_t dequantize_ll_coefficient(int32_t coefficient,
                                  int gcli,
                                  int gtli) {
    if (coefficient == 0 || gcli <= gtli) {
        return 0;
    }
    const uint32_t magnitude =
        static_cast<uint32_t>(coefficient < 0 ? -coefficient : coefficient) >> gtli;
    const int32_t factor = (1 << kW4Shift) + 1;
    const int32_t result = static_cast<int32_t>(magnitude) * factor << kW4Shift;
    return (coefficient < 0) ? -result : result;
}

void dequantize_coefficient_array(
    int32_t* NIKON_HE_RESTRICT coefficients,
    int coefficient_count,
    const uint8_t* NIKON_HE_RESTRICT gcli_values,
    int num_groups,
    int target_gtli,
    bool is_ll_band) {

    (void)is_ll_band;
    const int groups = (num_groups * 4 <= coefficient_count)
                       ? num_groups
                       : coefficient_count / 4;
    for (int g = 0; g < groups; ++g) {
        const int gcli = static_cast<int>(gcli_values[g]);
        int32_t* NIKON_HE_RESTRICT dst = coefficients + g * 4;
        if (gcli <= target_gtli) {
            dst[0] = dst[1] = dst[2] = dst[3] = 0;
            continue;
        }
        const int bit_plane_count = gcli - target_gtli;
        if (bit_plane_count < 1 || bit_plane_count > 15) {
            dst[0] = dst[1] = dst[2] = dst[3] = 0;
            continue;
        }
        const uint32_t scale =
            static_cast<uint32_t>(kMidpointScaleTable[bit_plane_count - 1]);
        const int shift = 16 - target_gtli;
        for (int c = 0; c < 4; ++c) {
            const int32_t v = dst[c];
            if (v == 0) {
                continue;
            }
            const uint32_t magnitude =
                static_cast<uint32_t>(v < 0 ? -v : v) >> target_gtli;
            const int32_t reconstructed =
                static_cast<int32_t>((static_cast<uint64_t>(magnitude) * scale) >> shift)
                << kW4Shift;
            dst[c] = (v < 0) ? -reconstructed : reconstructed;
        }
    }
}

}  // namespace nikon_he
