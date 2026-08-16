/* -*- C++ -*-
 * File: nikon_he_coefficient_decode.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE coefficient magnitude/sign decode implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE coefficient decode — implementation.
//
// See nikon_he_coefficient_decode.h for the public API.

#include "nikon_he_coefficient_decode.h"
#include "nikon_he_dequantize.h"

namespace nikon_he {

static inline void unpack_one_nibble(int32_t m[4], uint32_t nibble) {
    m[0] = (m[0] << 1) | static_cast<int32_t>((nibble >> 3) & 1u);
    m[1] = (m[1] << 1) | static_cast<int32_t>((nibble >> 2) & 1u);
    m[2] = (m[2] << 1) | static_cast<int32_t>((nibble >> 1) & 1u);
    m[3] = (m[3] << 1) | static_cast<int32_t>( nibble       & 1u);
}

void unpack_coefficient_magnitudes(
    BitReader& data_reader,
    const uint8_t* gcli_values,
    int gtli,
    int num_groups,
    int32_t* coefficients_out) {

    for (int g = 0; g < num_groups; ++g) {
        const int num_bitplanes = static_cast<int>(gcli_values[g]) - gtli;
        int32_t* dst = coefficients_out + g * 4;
        if (num_bitplanes <= 0) {
            dst[0] = dst[1] = dst[2] = dst[3] = 0;
            continue;
        }

        int32_t m[4] = {0, 0, 0, 0};
        int bp = 0;
        while (bp + 8 <= num_bitplanes) {
            uint32_t bits = data_reader.read_bits(32);
            for (int k = 0; k < 8; ++k) {
                unpack_one_nibble(m, bits >> 28);
                bits <<= 4;
            }
            bp += 8;
        }
        for (; bp < num_bitplanes; ++bp) {
            unpack_one_nibble(m, data_reader.read_bits(4));
        }

        dst[0] = m[0] << gtli;
        dst[1] = m[1] << gtli;
        dst[2] = m[2] << gtli;
        dst[3] = m[3] << gtli;
    }
}

void apply_sign_bits(
    BitReader& sign_reader,
    int32_t* coefficients,
    int coefficient_count) {

    for (int i = 0; i < coefficient_count; ++i) {
        if (coefficients[i] == 0) {
            continue;
        }
        if (sign_reader.read_bits(1) == 1) {
            coefficients[i] = -coefficients[i];
        }
    }
}

bool unpack_sign_and_dequantize(
    BitReader& data_reader,
    BitReader& sign_reader,
    const uint8_t* gcli_values,
    int gtli,
    int num_groups,
    int32_t* coefficients_out) {

    bool all_zero = true;
    for (int g = 0; g < num_groups; ++g) {
        const int gcli = static_cast<int>(gcli_values[g]);
        const int num_bitplanes = gcli - gtli;
        int32_t* dst = coefficients_out + g * 4;
        if (num_bitplanes <= 0) {
            dst[0] = dst[1] = dst[2] = dst[3] = 0;
            continue;
        }

        int32_t m[4] = {0, 0, 0, 0};
        int bp = 0;
        while (bp + 8 <= num_bitplanes) {
            uint32_t bits = data_reader.read_bits(32);
            for (int k = 0; k < 8; ++k) {
                unpack_one_nibble(m, bits >> 28);
                bits <<= 4;
            }
            bp += 8;
        }
        for (; bp < num_bitplanes; ++bp) {
            unpack_one_nibble(m, data_reader.read_bits(4));
        }

        m[0] <<= gtli;
        m[1] <<= gtli;
        m[2] <<= gtli;
        m[3] <<= gtli;

        for (int c = 0; c < 4; ++c) {
            if (m[c] != 0 && sign_reader.read_bits(1) == 1) {
                m[c] = -m[c];
            }
        }

        if (gcli <= gtli || num_bitplanes < 1 || num_bitplanes > 15) {
            dst[0] = dst[1] = dst[2] = dst[3] = 0;
            continue;
        }

        const uint32_t scale =
            static_cast<uint32_t>(kMidpointScaleTable[num_bitplanes - 1]);
        const int shift = 16 - gtli;
        for (int c = 0; c < 4; ++c) {
            const int32_t v = m[c];
            if (v == 0) {
                dst[c] = 0;
                continue;
            }
            all_zero = false;
            const uint32_t magnitude =
                static_cast<uint32_t>(v < 0 ? -v : v) >> gtli;
            const int32_t reconstructed =
                static_cast<int32_t>((static_cast<uint64_t>(magnitude) * scale) >> shift)
                << kW4Shift;
            dst[c] = (v < 0) ? -reconstructed : reconstructed;
        }
    }
    return all_zero;
}

}  // namespace nikon_he
