/* -*- C++ -*-
 * File: nikon_he_bit_reader.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE bitstream reader interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE bitstream reader.
//
// All bit-level reads are MSB-first big-endian. This reader consumes 32-bit
// big-endian words from the byte buffer and shifts them into a 64-bit
// register, MSB-justified. Reads past the end of the buffer return
// zero-padded bits.
//
// Hot methods live in this header so GCLI / coefficient decode can inline
// them. A 64-bit refill plus a clz-based unary walk avoids a per-bit loop
// on long runs of ones.

#ifndef LIBRAW_NIKON_HE_BIT_READER_H
#define LIBRAW_NIKON_HE_BIT_READER_H

#include "nikon_he_simd.h"
#include <cstddef>
#include <cstdint>

namespace nikon_he {

class BitReader {
public:
    BitReader() = default;

    // Initialize with a byte buffer. The reader does NOT own the buffer;
    // the caller must ensure it remains valid during the reader's lifetime.
    BitReader(const uint8_t* data, size_t length) { reset(data, length); }

    void reset(const uint8_t* data, size_t length) {
        base_ = data;
        cursor_ = data;
        end_ = data + length;
        register_ = 0;
        bits_available_ = 0;
    }

    // Read `count` bits MSB-first. count must be in [1, 32].
    // Past-end reads return available bits zero-padded on the LSB side.
    uint32_t read_bits(int count);

    // Read a plain unary code: count leading 1-bits, then consume the
    // terminating 0-bit. Returns the count of 1s (0..N).
    // At EOF without a terminator: returns the count of 1s seen so far.
    uint32_t read_unary();

    // Discard any partial byte so the next read is byte-aligned.
    void align_to_byte();

    // Total bytes consumed from the input buffer (excluding fractional
    // bits still pending in the register).
    size_t bytes_read() const;

    // True when all source bytes have been consumed. The register may
    // still contain zero-padded bits from a partial final word.
    bool exhausted() const { return cursor_ >= end_; }

private:
    void refill();

    const uint8_t* base_ = nullptr;
    const uint8_t* cursor_ = nullptr;
    const uint8_t* end_ = nullptr;
    uint64_t register_ = 0;
    int bits_available_ = 0;
};

inline void BitReader::refill() {
    uint32_t word = 0;
    const size_t remaining = static_cast<size_t>(end_ - cursor_);

    if (remaining >= 4) {
        word = (static_cast<uint32_t>(cursor_[0]) << 24)
             | (static_cast<uint32_t>(cursor_[1]) << 16)
             | (static_cast<uint32_t>(cursor_[2]) << 8)
             |  static_cast<uint32_t>(cursor_[3]);
        cursor_ += 4;
    } else if (remaining > 0) {
        for (size_t i = 0; i < remaining; ++i) {
            word = (word << 8) | cursor_[i];
        }
        word <<= static_cast<uint32_t>((4 - remaining) * 8);
        cursor_ = end_;
    } else {
        return;
    }

    const int shift = 32 - bits_available_;
    register_ |= static_cast<uint64_t>(word) << shift;
    bits_available_ += (remaining >= 4) ? 32 : static_cast<int>(remaining * 8);
}

inline uint32_t BitReader::read_bits(int count) {
    while (bits_available_ < count) {
        if (cursor_ >= end_) {
            const uint32_t result = static_cast<uint32_t>(register_ >> (64 - count));
            register_ = 0;
            bits_available_ = 0;
            return result;
        }
        refill();
    }
    const uint32_t result = static_cast<uint32_t>(register_ >> (64 - count));
    register_ <<= count;
    bits_available_ -= count;
    return result;
}

inline uint32_t BitReader::read_unary() {
    uint32_t count = 0;
    for (;;) {
        if (bits_available_ <= 0) {
            if (cursor_ >= end_) {
                return count;
            }
            refill();
            if (bits_available_ <= 0) {
                return count;
            }
        }
        // Valid bits sit at the top of register_. Leading ones of those
        // bits are the leading zeros of the inverted register.
        const int ones = count_leading_zeros64(~register_);
        if (ones >= bits_available_) {
            count += static_cast<uint32_t>(bits_available_);
            register_ = 0;
            bits_available_ = 0;
            continue;
        }
        const int consume = ones + 1;
        register_ <<= consume;
        bits_available_ -= consume;
        return count + static_cast<uint32_t>(ones);
    }
}

inline void BitReader::align_to_byte() {
    const int fractional = bits_available_ & 7;
    if (fractional == 0) return;
    register_ <<= fractional;
    bits_available_ -= fractional;
}

inline size_t BitReader::bytes_read() const {
    const size_t consumed = static_cast<size_t>(cursor_ - base_);
    const size_t held_in_register = static_cast<size_t>(bits_available_ >> 3);
    return consumed - held_in_register;
}

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_BIT_READER_H
