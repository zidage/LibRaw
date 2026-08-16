/* -*- C++ -*-
 * File: nikon_he_idwt_vertical.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE vertical inverse DWT implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE vertical 5/3 IDWT — implementation.
//
// See nikon_he_idwt_vertical.h for the public API.

#include "nikon_he_idwt_vertical.h"
#include "nikon_he_simd.h"

namespace nikon_he {

void ver_lift_lb_step(
    const int32_t* NIKON_HE_RESTRICT x0,
    int32_t* NIKON_HE_RESTRICT x1_out,
    int n,
    VerLiftStatePerLB& st) {

    int32_t* NIKON_HE_RESTRICT x2 = st.x2_carry;
    int32_t* NIKON_HE_RESTRICT x3 = st.x3_carry;

    switch (st.state) {
    case VerLiftState::kInit0:
        NIKON_HE_SIMD_LOOP
        for (int i = 0; i < n; ++i) {
            x2[i] = x0[i];
        }
        st.state = VerLiftState::kInit1;
        break;

    case VerLiftState::kInit1:
        NIKON_HE_SIMD_LOOP
        for (int i = 0; i < n; ++i) {
            x2[i] = (x0[i] << 2) - x2[i];
        }
        st.state = VerLiftState::kInit4;
        break;

    case VerLiftState::kInit4:
        NIKON_HE_SIMD_LOOP
        for (int i = 0; i < n; ++i) {
            const int32_t w = x2[i] - x0[i];
            x3[i] = (w + 1) >> 2;
            x2[i] = x0[i];
        }
        st.state = VerLiftState::kState7;
        break;

    case VerLiftState::kInit2:
        NIKON_HE_SIMD_LOOP
        for (int i = 0; i < n; ++i) {
            x2[i] = x0[i] << 2;
        }
        st.state = VerLiftState::kState5;
        break;

    case VerLiftState::kState5:
        NIKON_HE_SIMD_LOOP
        for (int i = 0; i < n; ++i) {
            const int32_t w = x2[i] - 2 * x0[i];
            x3[i] = (w + 1) >> 2;
            x2[i] = x0[i];
        }
        st.state = VerLiftState::kState7;
        break;

    case VerLiftState::kState7:
        if (x0 == nullptr) {
            NIKON_HE_SIMD_LOOP
            for (int i = 0; i < n; ++i) {
                x1_out[i] = x3[i];
            }
            st.state = VerLiftState::kState9;
        } else {
            NIKON_HE_SIMD_LOOP
            for (int i = 0; i < n; ++i) {
                const int32_t tmp = x3[i];
                x1_out[i] = tmp;
                x3[i] = tmp + 2 * x2[i];
                x2[i] = (x0[i] << 2) - x2[i];
            }
            st.state = VerLiftState::kState8;
        }
        break;

    case VerLiftState::kState8:
        NIKON_HE_SIMD_LOOP
        for (int i = 0; i < n; ++i) {
            int32_t w = x2[i] - x0[i];
            w = (w + 1) >> 2;
            x1_out[i] = (w + x3[i]) >> 1;
            x2[i] = x0[i];
            x3[i] = w;
        }
        st.state = VerLiftState::kState7;
        break;

    case VerLiftState::kState9:
        NIKON_HE_SIMD_LOOP
        for (int i = 0; i < n; ++i) {
            x1_out[i] = x3[i] + x2[i];
        }
        st.state = VerLiftState::kState11;
        break;

    case VerLiftState::kState11:
        break;

    default:
        break;
    }
}

}  // namespace nikon_he
