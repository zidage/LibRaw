/* -*- C++ -*-
 * File: nikon_he_gcli_decode.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE GCLI (group code-length info) decode implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE GCLI decode — implementation.

#include "nikon_he_gcli_decode.h"
#include <algorithm>
#include <cstring>

namespace nikon_he {

void decode_gcli_values(
    BitReader& sig_reader,
    BitReader& gcli_reader,
    int mode,
    int num_groups,
    int gtli,
    const uint8_t* predict_lut,
    const uint8_t* previous_gcli,
    uint8_t* gcli_output) {

    const int num_sig_blocks = (num_groups + 7) / 8;
    const uint8_t gtli_u8 = static_cast<uint8_t>(gtli);

    for (int block = 0; block < num_sig_blocks; ++block) {
        const int base = block * 8;
        const int block_size = std::min(8, num_groups - base);
        const uint32_t sig_bit = sig_reader.read_bits(1);

        if (sig_bit == 1) {
            if (mode == 0x71) {
                std::memset(gcli_output + base, gtli_u8, static_cast<size_t>(block_size));
            } else {
                for (int i = 0; i < block_size; ++i) {
                    const int prev = previous_gcli[base + i];
                    gcli_output[base + i] = static_cast<uint8_t>(std::max(prev, gtli));
                }
            }
        } else if (mode == 0x71) {
            for (int i = 0; i < block_size; ++i) {
                const uint32_t u = gcli_reader.read_unary();
                gcli_output[base + i] = static_cast<uint8_t>(gtli + u);
            }
        } else {
            for (int i = 0; i < block_size; ++i) {
                const uint32_t u = gcli_reader.read_unary();
                const int prev = previous_gcli[base + i];
                gcli_output[base + i] = lookup_prediction(
                    predict_lut, gtli, prev, static_cast<int>(u));
            }
        }
    }
}

}  // namespace nikon_he
