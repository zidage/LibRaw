# Nikon HE and HE* RAW Codestream

Date: 2026-06-21

This note describes the Nikon High-Efficiency (`HE`) and High-Efficiency★
(`HE*`) raw strip, and the decoder in this tree.

A Nikon NEF is a TIFF-like container. The raw SubIFD holds a Bayer mosaic.
`HE` and `HE*` compress that mosaic with a JPEG XS-like precinct stream:

- 5/3 wavelet transform
- bit-plane coding
- per-sub-band truncation

The camera menu names `HE` and `HE*` are rate budgets. They are not two
codestreams. The encoder sets `Bp`, `Br`, and `Dpb` on each precinct from the
local scene and the selected budget. The decoder uses one path for both names.

The decoder is in `src/decoders/nikon_he/` and
`src/decoders/nikon_he_decoder.cpp`. LibRaw routes a file to
`nikon_he_load_raw()` when the raw strip starts with `FF 10 FF 50`.

## Terms

| Term | Meaning in this decoder |
|---|---|
| Precinct | Spatial coding unit. One precinct has a 12-byte header and eight line blocks. |
| Line block (LB) | One of eight groups inside a precinct. Each LB has significance, GCLI, magnitude, and sign substreams. |
| Sub-band | One of 26 wavelet bands in a precinct (`sb0` … `sb25`). |
| GCLI | Group Code Length Information. Highest bit-plane that a group of four coefficients uses. |
| GTLI | Greatest Truncation Level Information. Number of low bit-planes the encoder drops for one sub-band. |
| `Bp` | Per-precinct precision / bit-plane regime. |
| `Br` | Per-precinct rate / refinement regime. |
| `Dpb` | 28 packed 2-bit depth hints in the precinct header. |
| Residual | `r[n] = x_dec[n] − x_ref[n]` on the 16-bit linear Bayer plane. |
| DN | Digital number. One code of the 16-bit raw sample. |
| LSB | Least significant bit of that 16-bit code. |
| MAE | Mean absolute error: mean of `\|r[n]\|`. |
| Peak residual | Maximum of `\|r[n]\|`. |

GCLI answers: how many bit-planes does this group occupy?
GTLI answers: how many low bit-planes did the encoder drop?

The encoder truncates the bottom `GTLI` bits. The decoder rebuilds those bits
with a dead-zone midpoint formula. See `nikon_he_dequantize.cpp`.

## Container

The raw SubIFD reports Nikon compression `34713`.

The raw strip starts with a JPEG XS-like marker:

```text
FF 10 FF 50
```

The precinct stream starts at:

```text
strip_offset + 0x9b
```

`src/metadata/tiff.cpp` reads four bytes at the strip offset. If those bytes
match the marker, LibRaw calls `nikon_he_load_raw()`. The test is the marker,
not a camera-model list.

Main files:

| File | Role |
|---|---|
| `nikon_he_decode.cpp` | Tile walk and precinct schedule |
| `nikon_he_precinct_header.cpp` | Precinct prefix and per-LB sizes |
| `nikon_he_precinct_decode.cpp` | Entropy decode for one precinct |
| `nikon_he_gtli_table.cpp` | `(Bp, Br) → 26 GTLI values`, plus derivation |
| `nikon_he_predecessor.h` | GCLI prediction state and p16 reset |
| `nikon_he_dequantize.cpp` | Dead-zone midpoint reconstruction |
| `nikon_he_idwt_horizontal.cpp` | Horizontal 5/3 inverse DWT |
| `nikon_he_idwt_vertical.cpp` | Vertical 5/3 inverse DWT |
| `nikon_he_bayer.cpp` | Merge wavelet output into the Bayer plane |

## Precinct

Each precinct starts with a 12-byte header:

| Field | Size | Meaning |
|---|---:|---|
| `size` | 24-bit big-endian | Payload size after the 12-byte header |
| `Bp` | 8-bit | Precision / bit-plane regime |
| `Br` | 8-bit | Rate / refinement regime |
| `Dpb` | 28 packed 2-bit fields | Extra per-line-block depth hints |

The payload has eight line blocks. Each line block has:

- a 7-byte mini-header
- a significance / unary substream
- a GCLI substream
- a coefficient magnitude substream
- a sign substream

Significance-substream length is computed per line block from that block’s
group count. One global byte count does not work for every LB. Z 50 II DX
files showed this.

A Z 8 / Z 9 full-frame plane is `8280 × 5520`. That plane has 1380 file
precincts. After every 16 file precincts the stream has a 6-byte pad.

A tile is 64 raw rows high. Tile `T` decodes file precincts
`T*16 … T*16+17`. Neighbour tiles share two overlap precincts.

If GCLI prediction state is wrong on that overlap, the residual looks like a
broken inverse DWT. The fault is the predictor, not the lift.

## One decoder for HE and HE*

The camera menu shows two names. The bytes show one syntax.

The encoder writes `Bp`, `Br`, and `Dpb` on every precinct. Those fields set
the GTLI row and the bit-plane budget for the local scene. A file that the
menu labels `HE` can mix several `Bp` values. A file that the menu labels
`HE*` can do the same.

Observed `Bp` counts:

| Sample | Menu name | Observed `Bp` counts |
|---|---|---|
| Z8 `he.NEF` | HE | `Bp5=1379`, `Bp4=1` |
| Z8 `he_star.NEF` | HE* | `Bp4=1371`, `Bp3=9` |
| Z9 `Nikon - Z9-  HE.NEF` | HE | `Bp4=1175`, `Bp3=121`, `Bp5=84` |
| Z9 `Nikon - Z9-  HE_star.NEF` | HE* | `Bp2=800`, `Bp3=550`, `Bp1=30` |
| Z6 III `Nikon - Z6_3 - HE.NEF` | HE | `Bp5=569`, `Bp4=210`, `Bp6=121`, `Bp3=110` |
| Z6 III `Nikon - Z6_3 - HE_star.NEF` | HE* | `Bp4=359`, `Bp2=316`, `Bp3=293`, `Bp5=42` |
| Z50 II `DSC_0012.NEF` | DX HE* | `Bp2=811`, `Bp3=118`, `Bp4=3` |

Z 8 HE* and Z 9 HE* do not use the same `Bp` range. The decoder does not
branch on the menu name. It does this:

1. Parse the common precinct syntax.
2. Map `(Bp, Br)` to 26 GTLI values.
3. Run the same GCLI, bit-plane, dequant, IDWT, and Bayer path.

## GTLI lookup

Each precinct has 26 sub-bands:

| Sub-band range | Contents |
|---|---|
| `sb0..5` | Pass A, deeper horizontal bands |
| `sb6..11` | Pass A, next horizontal bands |
| `sb12` | Pass A LL |
| `sb13..18` | Pass A, final horizontal bands |
| `sb19..20` | Pass B, first horizontal bands |
| `sb21..22` | Pass B, next horizontal bands |
| `sb23` | Pass B LL |
| `sb24..25` | Pass B, final horizontal bands |

`lookup_gtli_table(Bp, Br)` in `nikon_he_gtli_table.cpp` uses this order:

1. Exact table row.
2. Downward derivation from the nearest higher `Bp` at the same `Br`.
   Subtract the `Bp` gap from each GTLI. Clamp at 0.
3. Upward derivation from the nearest lower `Bp` at the same `Br`.
   Add the `Bp` gap to each GTLI.

Downward derivation is preferred. A low-`Bp` row may already sit at the
zero clamp. After a clamp, the pre-clamp GTLI is not recoverable.

At the same `Br`, one step of `Bp` moves GTLI by one bit-plane. That is the
JPEG XS-style truncation rule this table follows.

## GCLI predecessor reset

GCLI decode uses predecessor state. Two LL bands use cross-band prediction
(`sb12` and `sb23`). Other bands use zero prediction.

Tile-local precinct index 16 is the structural LL overlap precinct.
`should_reset_gcli()` resets all GCLI state when `precinct_index == 16`.
The function ignores `Bp` and `Br`. The same file precinct is decoded as
p16 of one tile and p0 of the next tile. A stale predictor on one side
creates a large residual on the tile boundary.

## Decode path

`LibRaw::nikon_he_load_raw()` reads the strip, skips `0x9b` bytes, and
calls `decode_nikon_he_image()`. That function:

1. Finds the raw SubIFD and the `FF 10 FF 50` marker.
2. Walks precincts from `strip_offset + 0x9b`.
3. Reads `size`, `Bp`, `Br`, and `Dpb` for each precinct.
4. Skips the 6-byte pad after every 16 file precincts.
5. Builds 64-row tiles with 18 precincts. The tile step is 16 precincts.
6. Creates a fresh GCLI predecessor state for each tile.
7. Resolves 26 GTLI values from `(Bp, Br)`.
8. Decodes significance, unary codes, and GCLI.
9. Decodes magnitude bit-planes and signs.
10. Reconstructs coefficients with the dead-zone midpoint dequantizer.
11. Runs the horizontal 5/3 inverse DWT.
12. Runs the vertical 5/3 inverse DWT. Two overflow stripes pass to the
    next tile.
13. Writes the result into LibRaw `raw_image`.
14. Sets `maximum` to `16383`.

Map:

```text
NEF/TIFF
  -> JPEG XS-like precinct stream
  -> GCLI / GTLI
  -> truncated bit-plane coefficients
  -> dead-zone midpoint reconstruction
  -> 5/3 inverse DWT
  -> 16-bit linear Bayer plane
```

## Residual check

Demosaic, white balance, colour matrices, and tone curves hide sample
errors. The check uses the linear Bayer plane after `open_file()` and
`unpack()`, before those steps.

Method:

1. Decode the NEF with LibRaw `open_file()` + `unpack()`.
2. Read the matching Adobe DNG Bayer plane.
3. Compute the residual `r[n] = x_dec[n] − x_ref[n]` on unsigned 16-bit
   samples.

The encoder drops `GTLI` bit-planes. The decoder rebuilds them with a
dead-zone midpoint. A second decoder that uses a different reconstruction
grid can differ by about one LSB. That residual is quantization /
truncation noise. It is not a tile-desync residual.

A structural fault (wrong GTLI row, stale GCLI state, bad IDWT carry)
produces a large peak residual, often near `16383`, and a high MAE.

Measured residual on the current samples:

| Sample | Zero residual | `\|r\| ≤ 1` LSB | `\|r\| ≤ 8` DN | Peak residual (DN) | MAE (DN) |
|---|---:|---:|---:|---:|---:|
| Z8 `he.NEF` | 86.8833% | 99.9955% | 99.9999% | 16 | 0.131 |
| Z8 `he_star.NEF` | 86.4527% | 99.9877% | 99.9984% | 38 | 0.136 |
| Z9 `Nikon - Z9-  HE.NEF` | 86.6909% | 99.9927% | 99.9992% | 46 | 0.133 |
| Z9 `Nikon - Z9-  HE_star.NEF` | 86.7453% | 99.9920% | 99.9990% | 47 | 0.133 |
| Z6 III `Nikon - Z6_3 - HE.NEF` | 86.6352% | 99.9936% | 99.9999% | 18 | 0.134 |
| Z6 III `Nikon - Z6_3 - HE_star.NEF` | 86.6356% | 99.9930% | 99.9996% | 35 | 0.134 |
| Z50 II `DSC_0012.NEF` | 86.8465% | 99.9666% | 99.9852% | 373 | 0.136 |

MAE is about `0.13` DN on a `0 … 16383` code range. Peak residual is tens
of DN on the full-frame files. That is the GTLI truncation floor.

On `DSC_0012.NEF`, every sample with `|r| > 8` DN sits in the last two raw
rows. After a 32-pixel border crop, the inner plane stays within 1 LSB.

`simple_dcraw -T -6` also writes a TIFF. The TIFF is a secondary check.
The Bayer residual is the primary check.

The raw.pixls.us Z 50 II HE / HE* files on this machine did not open. The
download may have failed. `DSC_0012.NEF` is the DX file used above.

## Rate

Uncompressed 16-bit planes:

```text
Z 8 / Z 9:  8280 * 5520 * 2 = 91,411,200 bytes
Z 6 III:    6064 * 4040 * 2 = 48,997,120 bytes
Z 50 II:    5600 * 3728 * 2 = 41,753,600 bytes
```

Sample file sizes:

| File | NEF size | Approx. bpp | Ratio vs raw plane |
|---|---:|---:|---:|
| `he.NEF` | 20,721,664 | 3.63 | 4.41:1 |
| `he_star.NEF` | 33,257,984 | 5.82 | 2.75:1 |
| `Nikon - Z9-  HE.NEF` | 24,839,168 | 4.35 | 3.68:1 |
| `Nikon - Z9-  HE_star.NEF` | 35,592,704 | 6.23 | 2.57:1 |
| `Nikon - Z6_3 - HE.NEF` | 13,429,248 | 4.39 | 3.65:1 |
| `Nikon - Z6_3 - HE_star.NEF` | 19,554,816 | 6.39 | 2.51:1 |
| `DSC_0012.NEF` | 16,011,776 | 6.14 | 2.61:1 |

Local decode times on 2026-06-21, reference implementation:

| Input | `he_dump` Bayer dump | `simple_dcraw -T -6` |
|---|---:|---:|
| `he.NEF` | 2.58 s | 12.11 s |
| `he_star.NEF` | 2.62 s | 11.89 s |
| `Nikon - Z9-  HE.NEF` | 2.48 s | 11.81 s |
| `Nikon - Z9-  HE_star.NEF` | 2.65 s | 11.35 s |
| `Nikon - Z6_3 - HE.NEF` | 2.80 s | 8.91 s |
| `Nikon - Z6_3 - HE_star.NEF` | 3.02 s | 8.57 s |

The current decoder uses a linear GTLI scan, scalar bit-plane unpack, scalar
dequant, and scalar 5/3 lifting. It does not use tile-level parallelism.

## Checked files

- Nikon Z 8 full-frame HE and HE*
- Nikon Z 9 full-frame HE and HE*
- Nikon Z f HE and HE*
- Nikon Z 50 II DX HE* (`DSC_0012.NEF`)
- Nikon Z 6 III full-frame HE and HE*

The raw.pixls.us Z 50 II files on this machine are not used as evidence.
The download may have failed.
