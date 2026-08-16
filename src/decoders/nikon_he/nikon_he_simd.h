/* -*- C++ -*-
 * File: nikon_he_simd.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE portable SIMD / OpenMP helpers

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Portable vectorization helpers for the Nikon HE decoder.
//
// Do not use x86 ISA intrinsics (_mm_*, <immintrin.h>, AVX). Those lock
// the decoder to one architecture. This header uses:
//   - compiler auto-vectorization hints
//   - OpenMP `simd` when the TU is built with OpenMP
//   - a portable count-leading-zeros helper (compiler builtin / bit scan)
//
// The same source builds on Windows MSVC (x64 and ARM64) and macOS
// (Apple Silicon and Intel) without an architecture-specific inner loop.

#ifndef LIBRAW_NIKON_HE_SIMD_H
#define LIBRAW_NIKON_HE_SIMD_H

#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(_OPENMP)
#include <omp.h>
#endif

namespace nikon_he {

#if defined(_MSC_VER)
#define NIKON_HE_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define NIKON_HE_RESTRICT __restrict__
#else
#define NIKON_HE_RESTRICT
#endif

// MSVC accepts `omp simd` only with /openmp:experimental. Alcedo and
// Makefile.msvc use /openmp, so MSVC gets loop(ivdep) and the optimizer
// still auto-vectorizes the independent integer lifts.
#if defined(_MSC_VER)
#define NIKON_HE_SIMD_LOOP __pragma(loop(ivdep))
#elif defined(_OPENMP)
#define NIKON_HE_SIMD_LOOP _Pragma("omp simd")
#elif defined(__clang__)
#define NIKON_HE_SIMD_LOOP _Pragma("clang loop vectorize(enable)")
#else
#define NIKON_HE_SIMD_LOOP
#endif

// Count leading zeros in a 64-bit word. 64 if x == 0.
// Not an x86 vector intrinsic: __builtin_clzll and _BitScanReverse64 are
// compiler helpers and exist on ARM as well as x86.
inline int count_leading_zeros64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return x ? __builtin_clzll(x) : 64;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64) || defined(_WIN64))
    unsigned long idx;
    return _BitScanReverse64(&idx, x) ? (63 - static_cast<int>(idx)) : 64;
#else
    int n = 0;
    if ((x >> 32) == 0) {
        n += 32;
        x <<= 32;
    }
    if ((x >> 48) == 0) {
        n += 16;
        x <<= 16;
    }
    if ((x >> 56) == 0) {
        n += 8;
        x <<= 8;
    }
    if ((x >> 60) == 0) {
        n += 4;
        x <<= 4;
    }
    if ((x >> 62) == 0) {
        n += 2;
        x <<= 2;
    }
    if ((x >> 63) == 0) {
        n += 1;
    }
    return n;
#endif
}

inline int openmp_max_threads() {
#if defined(_OPENMP)
    return omp_get_max_threads();
#else
    return 1;
#endif
}

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_SIMD_H
