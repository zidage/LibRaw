/* -*- C++ -*-
 * File: nikon_he_predecessor.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE cross-precinct GCLI predecessor state

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE precinct predecessor state — implementation.
//
// See nikon_he_predecessor.h for the public API.

#include "nikon_he_predecessor.h"
#include <algorithm>
#include <cstring>

namespace nikon_he {

void PrecinctPredecessorState::init(const SubbandConfig config[26]) {
    destroy();

    int others = 0;
    for (int i = 0; i < 26; i++) {
        ng_[i] = config[i].ng;
        if (i != 12 && i != 23) {
            others += ng_[i];
        }
    }

    rotation_buf_size_ = std::max(config[12].ng, config[23].ng);
    const int slab_bytes = others + 2 * rotation_buf_size_;
    if (slab_bytes > 0) {
        slab_ = new uint8_t[static_cast<size_t>(slab_bytes)]();
    }

    int off = 0;
    for (int i = 0; i < 26; i++) {
        if (i == 12 || i == 23) continue;
        if (ng_[i] > 0) {
            gcli_store_[i] = slab_ + off;
            off += ng_[i];
        } else {
            gcli_store_[i] = nullptr;
        }
    }

    if (rotation_buf_size_ > 0) {
        rotation_buf_a_ = slab_ + off;
        off += rotation_buf_size_;
        rotation_buf_b_ = slab_ + off;
    } else {
        rotation_buf_a_ = nullptr;
        rotation_buf_b_ = nullptr;
    }

    gcli_store_[12] = rotation_buf_b_;
    gcli_store_[23] = rotation_buf_a_;

    std::memset(fully_insig_, 0, sizeof(fully_insig_));
    precinct_index_ = 0;
}

void PrecinctPredecessorState::destroy() {
    delete[] slab_;
    slab_ = nullptr;
    rotation_buf_a_ = nullptr;
    rotation_buf_b_ = nullptr;
    rotation_buf_size_ = 0;
    for (int i = 0; i < 26; i++) {
        gcli_store_[i] = nullptr;
        ng_[i] = 0;
    }
    precinct_index_ = 0;
}

void PrecinctPredecessorState::reset_for_new_tile() {
    for (int i = 0; i < 26; i++) {
        if (gcli_store_[i] && ng_[i] > 0) {
            std::memset(gcli_store_[i], 0, static_cast<size_t>(ng_[i]));
        }
        fully_insig_[i] = false;
    }
    if (rotation_buf_a_) {
        std::memset(rotation_buf_a_, 0, static_cast<size_t>(rotation_buf_size_));
    }
    if (rotation_buf_b_) {
        std::memset(rotation_buf_b_, 0, static_cast<size_t>(rotation_buf_size_));
    }
    precinct_index_ = 0;
}

uint8_t* PrecinctPredecessorState::gcli_buffer(int sb_index) {
    if (sb_index < 0 || sb_index >= 26) return nullptr;
    return gcli_store_[sb_index];
}

const uint8_t* PrecinctPredecessorState::get_previous_gcli(int sb_index) const {
    if (sb_index < 0 || sb_index >= 26) return nullptr;

    if (sb_index == 12) {
        return rotation_buf_a_;
    } else if (sb_index == 23) {
        return rotation_buf_b_;
    } else {
        return gcli_store_[sb_index];
    }
}

void PrecinctPredecessorState::save_gcli(int sb_index, const uint8_t* gcli_values) {
    if (sb_index < 0 || sb_index >= 26) return;
    if (!gcli_store_[sb_index] || !gcli_values) return;

    int n = ng_[sb_index];
    if (n > 0) {
        std::memcpy(gcli_store_[sb_index], gcli_values, static_cast<size_t>(n));
    }
}

void PrecinctPredecessorState::set_fully_insig(int sb_index, bool flag) {
    if (sb_index >= 0 && sb_index < 26) {
        fully_insig_[sb_index] = flag;
    }
}

bool PrecinctPredecessorState::is_fully_insig(int sb_index) const {
    if (sb_index >= 0 && sb_index < 26) {
        return fully_insig_[sb_index];
    }
    return false;
}

void PrecinctPredecessorState::advance_precinct() {
    precinct_index_++;

    if (rotation_buf_b_) {
        std::memset(rotation_buf_b_, 0, static_cast<size_t>(rotation_buf_size_));
    }
}

void PrecinctPredecessorState::reset_gcli_state(const SubbandConfig config[26]) {
    (void)config;
    for (int i = 0; i < 26; i++) {
        if (gcli_store_[i] && ng_[i] > 0) {
            std::memset(gcli_store_[i], 0, static_cast<size_t>(ng_[i]));
        }
        fully_insig_[i] = false;
    }
}

}  // namespace nikon_he
