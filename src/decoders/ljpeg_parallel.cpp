/* -*- C++ -*-
 * File: ljpeg_parallel.cpp
 * Copyright (C) 2026 Yurun Zi
 *
   Parallel decoders for lossless JPEG DNG data (single-scan strips, tiles)

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Some cameras (Leica Q3, M11, SL3, ...) write the whole sensor as ONE
// lossless JPEG scan: one strip, no tiles, no restart markers. The stock
// lossless_dng_load_raw() must then decode ~60M Huffman symbols on one
// thread through the per-byte datastream get_char() (~1 s on a Q3 file).
//
// JPEG Huffman codes resynchronise on their own: decoding from an arbitrary
// bit offset soon lands on a true symbol boundary. So the scan can be split
// into chunks decoded in parallel:
//
//   0. Unstuff the scan (drop the 0x00 after each 0xFF) so bit offsets are
//      plain offsets into a byte array.
//   1. Each chunk decodes speculatively from its nominal start, records its
//      first kSyncPositions symbol starts and counts symbols to its end.
//   2. Chunk k keeps decoding past its end until it lands on a symbol start
//      that chunk k+1 also produced. From there both decodes agree, which
//      fixes chunk k+1's true start and every chunk's true symbol count.
//   3. Each chunk re-decodes from its verified start and writes its diffs
//      straight into raw_image at its global sample offset.
//   4. Undo the prediction. Predictor 1 makes column 0 chain down the rows
//      (a short serial pass); every other sample depends only on its left
//      neighbour in the same row, so rows finish in parallel.
//
// Tiled files take a simpler route (decode_tiles() below): every tile is its
// own JPEG stream, so tiles are just spread over the threads.
//
// Both results match lossless_dng_load_raw() bit for bit. A fast path only
// takes layouts it fully handles, and reports failure (the caller then runs
// the serial decoder) whenever the data looks corrupt or never resyncs.

// Algorithm code comes first: it needs only standard headers, and keeping it
// before dcraw_defs.h avoids LibRaw's macro-defined names (width, curve, ...).
// libraw_types.h (for LIBRAW_USE_OPENMP) must see the same build macros that
// dcraw_defs.h defines, since its include guard keeps this first expansion.
#define LIBRAW_LIBRARY_BUILD
#define LIBRAW_IO_REDEFINED
#include "../../libraw/libraw_types.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#if defined(_MSC_VER)
#include <stdlib.h>
#endif

namespace libraw_ljpeg_parallel
{

struct Params
{
  const uint8_t *scan = nullptr;  // entropy-coded bytes right after the SOS header
  size_t scan_bytes = 0;
  const uint16_t *huff = nullptr; // LibRaw decoder table: huff[0] = max code length
  int clrs = 0;                   // interleaved components per JPEG sample
  int bits = 0;                   // sample precision after the point transform
  unsigned row_samples = 0;       // output samples per row (jh.wide * clrs)
  unsigned rows = 0;
  const uint16_t *tone_curve = nullptr; // nullptr when the tone curve is identity
  int threads = 1;
  uint16_t *out = nullptr;         // row_samples * rows
  void (*check_cancel)(void *) = nullptr;
  void *cancel_ctx = nullptr;
};

namespace
{

const int kSyncPositions = 4096;           // symbol starts recorded per chunk
const uint64_t kMinChunkBits = 8ull << 20; // 1 MiB of scan data per chunk
const size_t kScanPadding = 16;            // window() may read past the last bit

inline uint64_t load_be64(const uint8_t *p)
{
  uint64_t v;
  memcpy(&v, p, sizeof v);
#if defined(_MSC_VER)
  return _byteswap_uint64(v);
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return v;
#else
  return __builtin_bswap64(v);
#endif
}

// At least 57 valid bits starting at bit offset pos, MSB first.
inline uint64_t window(const uint8_t *u, uint64_t pos)
{
  return load_be64(u + (pos >> 3)) << (pos & 7);
}

struct Symbol
{
  uint32_t bits; // bits consumed; 0 = invalid code
  int32_t diff;
};

// Same semantics as LibRaw::ljpeg_diff() for DNG >= 1.1 (SSSS 16 = -32768,
// no extra bits). SSSS > 16 cannot come from a valid stream.
inline Symbol decode_symbol(const uint16_t *lut, const uint8_t *u,
                            uint64_t pos)
{
  const uint64_t w = window(u, pos);
  const uint16_t e = lut[w >> 48];
  const uint32_t len = e >> 8, ssss = e & 0xff;
  Symbol s = {0, 0};
  if (!len || ssss > 16)
    return s;
  s.bits = len;
  if (ssss == 16)
    s.diff = -32768;
  else if (ssss)
  {
    int32_t diff = int32_t((w << len) >> (64 - ssss));
    if (!(diff >> (ssss - 1)))
      diff -= (1 << ssss) - 1;
    s.bits += ssss;
    s.diff = diff;
  }
  return s;
}

// While unsynced, an invalid prefix just means a wrong alignment.
inline uint64_t speculative_step(const uint16_t *lut, const uint8_t *u,
                                 uint64_t pos)
{
  const uint32_t n = decode_symbol(lut, u, pos).bits;
  return pos + (n ? n : 1);
}

inline void check_cancel(const Params &p)
{
  if (p.check_cancel)
    p.check_cancel(p.cancel_ctx);
}

// Copies the scan into u without byte stuffing, up to the first marker.
// Returns the number of unstuffed bytes.
size_t unstuff(const Params &p, std::unique_ptr<uint8_t[]> &u)
{
  const uint8_t *s = p.scan;
  const size_t n = p.scan_bytes;
  const int blocks = p.threads > 1 ? p.threads * 4 : 1;
  const size_t block_bytes = (n + blocks - 1) / blocks;
  std::vector<size_t> kept(blocks + 1, 0), marker(blocks, n);

  // A 0x00 directly after 0xFF is stuffing; 0xFF followed by anything else
  // starts a marker, which ends the scan.
#ifdef LIBRAW_USE_OPENMP
#pragma omp parallel for schedule(static) num_threads(p.threads) if (blocks > 1)
#endif
  for (int b = 0; b < blocks; b++)
  {
    const size_t lo = (std::min)(n, size_t(b) * block_bytes);
    const size_t hi = (std::min)(n, lo + block_bytes);
    size_t count = 0, k;
    for (k = lo; k < hi; k++)
    {
      const uint8_t c = s[k];
      if (c == 0xff && (k + 1 >= n || s[k + 1] != 0))
        break;
      count += !(c == 0 && k && s[k - 1] == 0xff);
    }
    kept[b + 1] = count;
    marker[b] = k < hi ? k : n;
  }

  size_t scan_end = n;
  int last_block = blocks - 1;
  for (int b = 0; b < blocks; b++)
    if (marker[b] < n)
    {
      scan_end = marker[b];
      last_block = b;
      break;
    }
  for (int b = 0; b < blocks; b++)
    kept[b + 1] = kept[b] + (b <= last_block ? kept[b + 1] : 0);

  // Only the padding needs zeroing; every other byte is written below.
  u.reset(new uint8_t[kept[blocks] + kScanPadding]);
  memset(u.get() + kept[blocks], 0, kScanPadding);
#ifdef LIBRAW_USE_OPENMP
#pragma omp parallel for schedule(static) num_threads(p.threads) if (blocks > 1)
#endif
  for (int b = 0; b <= last_block; b++)
  {
    const size_t lo = (std::min)(scan_end, size_t(b) * block_bytes);
    const size_t hi = (std::min)(scan_end, lo + block_bytes);
    uint8_t *dst = u.get() + kept[b];
    for (size_t k = lo; k < hi; k++)
      if (!(s[k] == 0 && k && s[k - 1] == 0xff))
        *dst++ = s[k];
  }
  return kept[blocks];
}

} // namespace

// Returns false when the data does not decode cleanly; out is then garbage
// and the caller must rerun the serial decoder.
bool decode(const Params &p)
{
  const int max_len = p.huff[0];
  if (max_len < 1 || max_len > 16 || p.clrs < 1 || p.clrs > 4 ||
      p.bits < 1 || p.bits > 16 || !p.row_samples || !p.rows)
    return false;

  // 16-bit peek table built from LibRaw's own decoder so both paths agree.
  std::vector<uint16_t> lut(1 << 16);
  for (uint32_t x = 0; x < lut.size(); x++)
    lut[x] = p.huff[1 + (x >> (16 - max_len))];

  std::unique_ptr<uint8_t[]> u;
  const uint64_t scan_bits = uint64_t(unstuff(p, u)) * 8;
  const uint8_t *ub = u.get();
  const uint64_t total = uint64_t(p.row_samples) * p.rows;
  check_cancel(p);

  // Chunk layout: one chunk means a plain serial decode (no sync phase).
  int chunks = 1;
  if (p.threads > 1)
    chunks = int((std::max<uint64_t>)(
        1, (std::min<uint64_t>)(uint64_t(p.threads) * 2, scan_bits / kMinChunkBits)));
  const uint64_t chunk_bits = (scan_bits + chunks - 1) / chunks;

  std::vector<uint64_t> start_pos(chunks + 1, 0), offset(chunks + 1, 0);
  if (chunks == 1)
    offset[1] = total;
  else
  {
    // Phase 1: speculative decode of every chunk.
    std::vector<std::vector<uint64_t> > sync(chunks);
    std::vector<uint64_t> count(chunks), end_pos(chunks);
    std::vector<uint64_t> start_idx(chunks, 0), overflow(chunks, 0);
    std::vector<char> synced(chunks, 1);
#ifdef LIBRAW_USE_OPENMP
#pragma omp parallel for schedule(static, 1) num_threads(p.threads)
#endif
    for (int k = 0; k < chunks; k++)
    {
      uint64_t pos = uint64_t(k) * chunk_bits;
      const uint64_t end = (std::min)(scan_bits, pos + chunk_bits);
      uint64_t n = 0;
      std::vector<uint64_t> &positions = sync[k];
      positions.reserve(kSyncPositions);
      for (; pos < end; n++)
      {
        if (n < uint64_t(kSyncPositions))
          positions.push_back(pos);
        pos = speculative_step(lut.data(), ub, pos);
      }
      count[k] = n;
      end_pos[k] = pos;
    }
    check_cancel(p);

    // Phase 2: run each chunk past its end until it meets the next chunk.
#ifdef LIBRAW_USE_OPENMP
#pragma omp parallel for schedule(static, 1) num_threads(p.threads)
#endif
    for (int k = 0; k < chunks - 1; k++)
    {
      const std::vector<uint64_t> &next = sync[k + 1];
      uint64_t pos = end_pos[k], n = 0;
      size_t j = 0;
      for (;;)
      {
        while (j < next.size() && next[j] < pos)
          j++;
        if (j < next.size() && next[j] == pos)
          break;
        if (j >= next.size() || pos > scan_bits)
        {
          synced[k] = 0;
          break;
        }
        pos = speculative_step(lut.data(), ub, pos);
        n++;
      }
      overflow[k] = n;
      start_pos[k + 1] = pos;
      start_idx[k + 1] = j;
    }
    for (int k = 0; k < chunks - 1; k++)
      if (!synced[k])
        return false;

    for (int k = 0; k < chunks; k++)
      offset[k + 1] = offset[k] + count[k] - start_idx[k] + overflow[k];
    // The last chunk also counts symbols decoded from trailing pad bits.
    if (offset[chunks - 1] >= total || offset[chunks] < total)
      return false;
    offset[chunks] = total;
    check_cancel(p);
  }

  // Phase 3: decode every chunk from its verified start, diffs into out.
  std::vector<char> ok(chunks, 1);
#ifdef LIBRAW_USE_OPENMP
#pragma omp parallel for schedule(static, 1) num_threads(p.threads) if (chunks > 1)
#endif
  for (int k = 0; k < chunks; k++)
  {
    uint64_t pos = start_pos[k];
    uint16_t *dst = p.out + offset[k];
    const uint64_t n = offset[k + 1] - offset[k];
    for (uint64_t i = 0; i < n; i++)
    {
      const Symbol s = decode_symbol(lut.data(), ub, pos);
      pos += s.bits;
      if (!s.bits || pos > scan_bits)
      {
        ok[k] = 0;
        break;
      }
      dst[i] = uint16_t(s.diff);
    }
    if (k < chunks - 1 && pos != start_pos[k + 1])
      ok[k] = 0;
  }
  for (int k = 0; k < chunks; k++)
    if (!ok[k])
      return false;
  check_cancel(p);

  // Phase 4: undo the prediction. Values are 16-bit like LibRaw's row buffer.
  const unsigned stride = p.row_samples, clrs = unsigned(p.clrs);
  int pred[4];
  for (unsigned c = 0; c < clrs; c++)
    pred[c] = 1 << (p.bits - 1);
  unsigned seen = 0;
  for (unsigned r = 0; r < p.rows; r++)
    for (unsigned c = 0; c < clrs; c++)
    {
      uint16_t &v = p.out[size_t(r) * stride + c];
      pred[c] = uint16_t(pred[c] + int16_t(v));
      v = uint16_t(pred[c]);
      seen |= v;
    }
  const int rows = int(p.rows);
#ifdef LIBRAW_USE_OPENMP
#pragma omp parallel for schedule(static) num_threads(p.threads) reduction(|: seen) if (p.threads > 1)
#endif
  for (int r = 0; r < rows; r++)
  {
    uint16_t *row = p.out + size_t(r) * stride;
    unsigned row_seen = 0;
    for (unsigned x = clrs; x < stride; x++)
    {
      row[x] = uint16_t(row[x - clrs] + row[x]);
      row_seen |= row[x];
    }
    if (p.tone_curve)
      for (unsigned x = 0; x < stride; x++)
        row[x] = p.tone_curve[row[x]];
    seen |= row_seen;
  }
  // Out-of-range samples: let the serial decoder raise LibRaw's data error.
  return (seen >> p.bits) == 0;
}


// ---------------------------------------------------------------------------
// Tiled DNGs (Adobe DNG Converter and many in-camera DNGs) store the sensor as
// hundreds of independent lossless JPEG tiles, which lossless_dng_load_raw()
// decodes one after another. Each tile is a complete JPEG stream, so tiles
// decode in parallel without any resynchronisation. Every tile repeats what
// ljpeg_start() / ljpeg_row_unrolled() / adobe_copy_pixel() would do for it.

struct TileLayout
{
  unsigned image_width = 0, image_height = 0; // raw_width / raw_height
  unsigned tile_w = 0, tile_h = 0;
  bool samples_per_pixel_are_clrs = false; // filters || colors == 1: jwide *= clrs
  int samples = 1;                        // tiff_samples
  bool short_diff16 = true;               // SSSS 16 = -32768 (all but DNG 1.0)
  const uint16_t *tone_curve = nullptr;   // nullptr when identity
  uint16_t *cfa_out = nullptr;            // raw_image (samples == 1)
  uint16_t (*image4)[4] = nullptr;        // otherwise: interleaved 4-channel image
  int threads = 1;
};

struct TileSpan
{
  const uint8_t *data = nullptr;
  size_t bytes = 0;
  unsigned row0 = 0, col0 = 0;
};

namespace
{

const int kFastBits = 11;

// Canonical Huffman table: an 11-bit lookup for short codes plus per-length
// code ranges for long ones. For a table that is not overfull this decodes
// exactly like LibRaw's make_decoder_ref() lookup, overfull tables are refused.
struct HuffTable
{
  uint16_t fast[1 << kFastBits]; // (len << 8) | symbol, 0 = not a short code
  int32_t maxcode[17];            // largest code of each length, -1 if none
  int32_t valoff[17];             // symbol index = valoff[len] + code
  uint8_t vals[256];
  int maxlen;
};

bool build_table(const uint8_t *counts, const uint8_t *symbols, HuffTable &t)
{
  memset(t.fast, 0, sizeof t.fast);
  int max = 16;
  while (max && !counts[max - 1])
    max--;
  if (!max)
    return false;
  t.maxlen = max;
  int32_t code = 0, k = 0;
  for (int len = 1; len <= 16; len++)
  {
    const int n = counts[len - 1];
    // Overfull (LibRaw would silently truncate it) or more than 256 symbols:
    // refuse before anything is written out of range.
    if (code + n > (1 << len) || k + n > 256)
      return false;
    t.valoff[len] = k - code;
    for (int i = 0; i < n; i++, k++, code++)
    {
      t.vals[k] = symbols[k];
      if (len <= kFastBits)
      {
        const int lo = code << (kFastBits - len), hi = (code + 1) << (kFastBits - len);
        for (int x = lo; x < hi; x++)
          t.fast[x] = uint16_t((len << 8) | symbols[k]);
      }
    }
    t.maxcode[len] = n ? code - 1 : -1;
    code <<= 1;
  }
  return true;
}

// Decodes one Huffman symbol from w; returns the code length, 0 if invalid.
inline int huff_decode(const HuffTable &t, uint64_t w, int &symbol)
{
  const uint16_t e = t.fast[w >> (64 - kFastBits)];
  if (e)
  {
    symbol = e & 0xff;
    return e >> 8;
  }
  for (int len = kFastBits + 1; len <= t.maxlen; len++)
  {
    const int32_t code = int32_t(w >> (64 - len));
    if (code <= t.maxcode[len])
    {
      symbol = t.vals[t.valoff[len] + code];
      return len;
    }
  }
  return 0;
}

struct TileHeader
{
  int bits = 0, high = 0, wide = 0, clrs = 0, psv = 0;
  const HuffTable *huff[6] = {};
  size_t scan_offset = 0;
};

// The subset of ljpeg_start() a lossless DNG tile may use: SOF3 without
// subsampling, DHT, SOS, DQT (ignored). Anything else is left to the serial
// decoder, including restart markers.
bool parse_tile_header(const uint8_t *d, size_t n, HuffTable *tables,
                       TileHeader &h)
{
  if (n < 2 || d[1] != 0xd8)
    return false;
  bool defined[20] = {};
  bool have_sof = false;
  size_t i = 2;
  for (int cnt = 0;; cnt++)
  {
    if (cnt > 1024 || i + 4 > n)
      return false;
    const unsigned tag = (d[i] << 8) | d[i + 1];
    const int len = int((d[i + 2] << 8) | d[i + 3]) - 2;
    i += 4;
    if (tag <= 0xff00 || len < 0 || i + size_t(len) > n)
      return false;
    const uint8_t *s = d + i;
    switch (tag)
    {
    case 0xffc3:
      if (len < 8 || (((s[7] >> 4) * (s[7] & 15) - 1) & 3))
        return false; // sraw (subsampled) layouts stay serial
      h.bits = s[0];
      h.high = (s[1] << 8) | s[2];
      h.wide = (s[3] << 8) | s[4];
      h.clrs = s[5];
      have_sof = true;
      break;
    case 0xffc0:
    case 0xffc1:
    case 0xffdd:
      return false;
    case 0xffc4:
    {
      const uint8_t *dp = s, *end = s + len;
      while (dp < end && !(*dp & -20))
      {
        const int c = *dp++;
        if (end - dp < 16)
          return false;
        const uint8_t *counts = dp;
        dp += 16;
        int total = 0;
        for (int k = 0; k < 16; k++)
          total += counts[k];
        if (end - dp < total)
          return false;
        // Only tables 0..3 can be selected by a component (huff[c], c < 6,
        // falls back to the previous slot); 16..19 are never used.
        if (c < 4 && !build_table(counts, dp, tables[c]))
          return false;
        defined[c] = true;
        dp += total;
      }
      break;
    }
    case 0xffda:
      if (len < 1 || 3 + 2 * s[0] >= len)
        return false;
      h.psv = s[1 + s[0] * 2];
      h.bits -= s[3 + s[0] * 2] & 15;
      break;
    }
    i += size_t(len);
    if (tag == 0xffda)
      break;
  }
  if (!have_sof || h.bits < 1 || h.bits > 16 || h.clrs < 1 || h.clrs > 6 ||
      !h.high || !h.wide || !defined[0])
    return false;
  // FORC(19) if (!huff[c + 1]) huff[c + 1] = huff[c];
  const HuffTable *cur = &tables[0];
  for (int c = 0; c < h.clrs; c++)
  {
    if (c < 4 && defined[c])
      cur = &tables[c];
    h.huff[c] = cur;
  }
  h.scan_offset = i;
  return true;
}

size_t unstuff_serial(const uint8_t *s, size_t n, uint8_t *dst)
{
  uint8_t *o = dst;
  for (size_t k = 0; k < n; k++)
  {
    const uint8_t c = s[k];
    if (c == 0xff)
    {
      if (k + 1 >= n || s[k + 1] != 0)
        break; // marker
      *o++ = 0xff;
      k++;
      continue;
    }
    *o++ = c;
  }
  return size_t(o - dst);
}

struct TileScratch
{
  HuffTable tables[4];
  std::vector<uint8_t> bits;
  std::vector<uint16_t> rows;
};

bool decode_tile(const TileLayout &L, const TileSpan &span, TileScratch &w)
{
  TileHeader h;
  if (!parse_tile_header(span.data, span.bytes, w.tables, h))
    return false;
  const unsigned clrs = unsigned(h.clrs), wide = unsigned(h.wide);
  const unsigned jwide = L.samples_per_pixel_are_clrs ? wide * clrs : wide;
  if (jwide != L.tile_w || unsigned(h.high) > L.tile_h ||
      wide * clrs != jwide * unsigned(L.samples))
    return false;

  const size_t scan_bytes = span.bytes - h.scan_offset;
  w.bits.resize(scan_bytes + kScanPadding);
  const size_t ubytes = unstuff_serial(span.data + h.scan_offset, scan_bytes, w.bits.data());
  memset(w.bits.data() + ubytes, 0, kScanPadding);
  const uint8_t *ub = w.bits.data();
  const uint64_t scan_bits = uint64_t(ubytes) * 8;

  const unsigned row_samples = wide * clrs;
  w.rows.assign(size_t(row_samples) * 2, 0);
  int vpred[6];
  for (unsigned c = 0; c < clrs; c++)
    vpred[c] = 1 << (h.bits - 1);
  uint64_t pos = 0;
  unsigned seen = 0;
  for (unsigned jrow = 0; jrow < unsigned(h.high); jrow++)
  {
    uint16_t *cur = w.rows.data() + size_t(row_samples) * (jrow & 1);
    const uint16_t *up = w.rows.data() + size_t(row_samples) * ((jrow + 1) & 1);
    for (unsigned x = 0; x < row_samples; x++)
    {
      const unsigned c = x % clrs;
      const uint64_t win = window(ub, pos);
      int ssss;
      const int len = huff_decode(*h.huff[c], win, ssss);
      if (!len || ssss > 16)
        return false;
      int diff = 0;
      pos += len;
      if (ssss == 16 && L.short_diff16)
        diff = -32768;
      else if (ssss)
      {
        diff = int((win << len) >> (64 - ssss));
        if (!(diff >> (ssss - 1)))
          diff -= (1 << ssss) - 1;
        pos += ssss;
      }
      if (pos > scan_bits)
        return false;
      int pred;
      if (x < clrs)
        pred = (vpred[c] += diff) - diff;
      else
      {
        pred = cur[x - clrs];
        if (jrow)
          switch (h.psv)
          {
          case 1:
            break;
          case 2:
            pred = up[x];
            break;
          case 3:
            pred = up[x - clrs];
            break;
          case 4:
            pred = pred + up[x] - up[x - clrs];
            break;
          case 5:
            pred = pred + ((up[x] - up[x - clrs]) >> 1);
            break;
          case 6:
            pred = up[x] + ((pred - up[x - clrs]) >> 1);
            break;
          case 7:
            pred = (pred + up[x]) >> 1;
            break;
          default:
            pred = 0;
          }
      }
      cur[x] = uint16_t(pred + diff);
      seen |= cur[x];
    }

    // adobe_copy_pixel(): one JPEG row is one tile row because jwide equals
    // tile_width; pixels past the image edge are dropped.
    const unsigned row = span.row0 + jrow;
    if (row >= L.image_height)
      continue;
    const unsigned ncols = span.col0 < L.image_width
                               ? (std::min)(jwide, L.image_width - span.col0)
                               : 0;
    const uint16_t *rp = cur;
    if (L.cfa_out)
    {
      uint16_t *dst = L.cfa_out + size_t(row) * L.image_width + span.col0;
      for (unsigned x = 0; x < ncols; x++)
        dst[x] = L.tone_curve ? L.tone_curve[rp[x]] : rp[x];
    }
    else
    {
      uint16_t(*dst)[4] = L.image4 + size_t(row) * L.image_width + span.col0;
      const unsigned ts = unsigned(L.samples);
      for (unsigned x = 0; x < ncols; x++, rp += ts)
        for (unsigned c = 0; c < ts; c++)
          dst[x][c] = L.tone_curve ? L.tone_curve[rp[c]] : rp[c];
    }
  }
  // Out-of-range samples: the serial decoder raises LibRaw's data error.
  return (seen >> h.bits) == 0;
}

} // namespace

bool decode_tiles(const TileLayout &L, const TileSpan *tiles, int count)
{
  std::vector<char> ok(size_t(count), 0);
#ifdef LIBRAW_USE_OPENMP
#pragma omp parallel num_threads(L.threads) if (L.threads > 1)
#endif
  {
    std::unique_ptr<TileScratch> scratch;
    try
    {
      scratch.reset(new TileScratch);
    }
    catch (const std::bad_alloc &)
    {
    }
#ifdef LIBRAW_USE_OPENMP
#pragma omp for schedule(dynamic, 4)
#endif
    for (int t = 0; t < count; t++)
    {
      if (!scratch)
        continue;
      try
      {
        ok[size_t(t)] = decode_tile(L, tiles[t], *scratch) ? 1 : 0;
      }
      catch (const std::bad_alloc &)
      {
      }
    }
  }
  for (int t = 0; t < count; t++)
    if (!ok[size_t(t)])
      return false;
  return true;
}

} // namespace libraw_ljpeg_parallel

#include "../../internal/dcraw_defs.h"

int LibRaw::lossless_dng_single_scan_load_raw(struct jhead *jh, unsigned jwide)
{
  // Exactly the layout where lossless_dng_load_raw() maps JPEG samples 1:1
  // onto raw_image rows: one strip, full width, predictor 1, no restarts,
  // and one Huffman table shared by every component.
  if (jh->algo != 0xc3 || jh->sraw || jh->psv != 1 || jh->restart != INT_MAX)
    return 0;
  if (!raw_image || tiff_samples != 1 || !(filters || colors == 1))
    return 0;
  if (jwide != raw_width || unsigned(jh->high) != raw_height ||
      tile_width < raw_width || tile_length != INT_MAX)
    return 0;
  if (INT64(jh->high) * jh->wide >= INT_MAX || jh->clrs < 1 || jh->clrs > 4)
    return 0;
  if (dng_version && dng_version < 0x1010000)
    return 0;
  if (!jh->huff[0])
    return 0;
  for (int c = 1; c < jh->clrs; c++)
    if (jh->huff[c] != jh->huff[0])
      return 0;

  const INT64 scan_pos = ftell(ifp);
  const INT64 strip_end = INT64(data_offset) + data_size;
  if (data_size <= 0 || strip_end <= scan_pos || strip_end > ifp->size() ||
      strip_end - scan_pos >= INT_MAX)
    return 0;

  bool identity_curve = true;
  for (int i = 0; i < 0x10000 && identity_curve; i++)
    identity_curve = curve[i] == i;

  libraw_ljpeg_parallel::Params p;
  p.huff = jh->huff[0];
  p.clrs = jh->clrs;
  p.bits = jh->bits;
  p.row_samples = raw_width;
  p.rows = raw_height;
  p.tone_curve = identity_curve ? nullptr : curve;
  p.out = raw_image;
#ifdef LIBRAW_USE_OPENMP
  p.threads = omp_get_max_threads() > 1 ? omp_get_max_threads() : 1;
#endif
  p.check_cancel = [](void *self) { static_cast<LibRaw *>(self)->checkCancel(); };
  p.cancel_ctx = this;

  bool decoded = false;
  try
  {
    const size_t scan_bytes = size_t(strip_end - scan_pos);
    std::unique_ptr<uint8_t[]> scan(new uint8_t[scan_bytes]);
    if (fread(scan.get(), 1, scan_bytes, ifp) == int(scan_bytes))
    {
      p.scan = scan.get();
      p.scan_bytes = scan_bytes;
      decoded = libraw_ljpeg_parallel::decode(p);
    }
  }
  catch (const std::bad_alloc &)
  {
    decoded = false;
  }
  if (!decoded)
    fseek(ifp, scan_pos, SEEK_SET);
  return decoded;
}

int LibRaw::lossless_dng_tiled_load_raw()
{
  // Same tile walk as lossless_dng_load_raw(): row-major tiles, one 32-bit
  // offset per tile read from data_offset.
  if (!tile_width || !tile_length || tile_width >= INT_MAX ||
      tile_length >= INT_MAX || tile_width > raw_width)
    return 0;
  if (tiff_samples < 1 || tiff_samples > 4 || tiff_samples == 2)
    return 0;
  if (raw_image ? tiff_samples != 1 : !image)
    return 0;
  const INT64 tiles_across = (INT64(raw_width) + tile_width - 1) / tile_width;
  const INT64 tiles_down = (INT64(raw_height) + tile_length - 1) / tile_length;
  const INT64 count = tiles_across * tiles_down;
  if (count < 2 || count > (1 << 20))
    return 0;

  const INT64 table_pos = ftell(ifp);
  const INT64 file_size = ifp->size();
  bool decoded = false;
  try
  {
    std::vector<INT64> offsets(static_cast<size_t>(count));
    for (INT64 t = 0; t < count; t++)
      offsets[size_t(t)] = get4();

    // The parser does not keep tile byte counts: each tile ends where the
    // next higher tile offset starts, the last one gets a generous bound.
    // A tile cut short fails to decode, which sends the image to the serial
    // path, so the bound only has to be large enough in practice.
    std::vector<INT64> sorted(offsets);
    std::sort(sorted.begin(), sorted.end());
    INT64 max_span = 0;
    for (size_t i = 0; i + 1 < sorted.size(); i++)
      max_span = (std::max)(max_span, sorted[i + 1] - sorted[i]);
    const INT64 first = sorted.front();
    const INT64 last_end =
        (std::min)(file_size, sorted.back() + 4 * max_span + 65536);
    if (first > 0 && sorted.back() < file_size && last_end - first < INT_MAX)
    {
      const size_t span_bytes = size_t(last_end - first);
      std::unique_ptr<uint8_t[]> data(new uint8_t[span_bytes]);
      fseek(ifp, first, SEEK_SET);
      if (fread(data.get(), 1, span_bytes, ifp) == int(span_bytes))
      {
        std::vector<libraw_ljpeg_parallel::TileSpan> spans(static_cast<size_t>(count));
        for (INT64 t = 0; t < count; t++)
        {
          const INT64 off = offsets[size_t(t)];
          std::vector<INT64>::const_iterator next =
              std::upper_bound(sorted.begin(), sorted.end(), off);
          const INT64 tile_end = next == sorted.end() ? last_end : *next;
          libraw_ljpeg_parallel::TileSpan &s = spans[size_t(t)];
          s.data = data.get() + (off - first);
          s.bytes = size_t(tile_end - off);
          s.row0 = unsigned((t / tiles_across) * tile_length);
          s.col0 = unsigned((t % tiles_across) * tile_width);
        }

        bool identity_curve = true;
        for (int i = 0; i < 0x10000 && identity_curve; i++)
          identity_curve = curve[i] == i;

        libraw_ljpeg_parallel::TileLayout L;
        L.image_width = raw_width;
        L.image_height = raw_height;
        L.tile_w = tile_width;
        L.tile_h = tile_length;
        L.samples_per_pixel_are_clrs = filters || colors == 1;
        L.samples = tiff_samples;
        L.short_diff16 = !dng_version || dng_version >= 0x1010000;
        L.tone_curve = identity_curve ? nullptr : curve;
        L.cfa_out = raw_image;
        L.image4 = raw_image ? nullptr : image;
#ifdef LIBRAW_USE_OPENMP
        L.threads = omp_get_max_threads() > 1 ? omp_get_max_threads() : 1;
#endif
        checkCancel();
        decoded = libraw_ljpeg_parallel::decode_tiles(L, spans.data(), int(count));
      }
    }
  }
  catch (const std::bad_alloc &)
  {
    decoded = false;
  }
  if (!decoded)
    fseek(ifp, table_pos, SEEK_SET);
  return decoded;
}
