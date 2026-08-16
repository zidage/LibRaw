# Nikon HE / HE* Decoder Handoff

Date: 2026-06-20
Branch: `codex/merge-pr-825-826`

This note records the current evidence for Nikon HE / HE* decoding work in this
LibRaw checkout. The findings are based on raw-plane comparisons against Adobe
DNG reference output, not visual inspection.

Public English algorithm write-up: `doc/nikon_he_public_algorithm.md`.

## Latest Status

Latest validated state after the 2026-06-21 Z50 II follow-up:

- Z9 `HE.NEF` is now in the same quantization-error range as the known-good Z8
  samples: exact `86.6909%`, within +/-1 `99.9927%`, within +/-8 `99.9992%`,
  max abs diff `46`, mean abs diff `0.133`.
- Z9 `HE_star.NEF` is now in the same quantization-error range:
  exact `86.7453%`, within +/-1 `99.9920%`, within +/-8 `99.9990%`,
  max abs diff `47`, mean abs diff `0.133`.
- Z6 III `HE.NEF` and `HE_star.NEF` also decode to the same quantization-error
  range. Z6 III `HE.NEF` is the key robustness sample because it dynamically
  uses `Bp=6` and low-Br `Bp=5` inside a normal HE file.
- Z50 II `DSC_0012.NEF` validates the DX path. It routes to
  `nikon_he_load_raw()` and compares to the adjacent ACR DNG at exact
  `86.8465%`, within +/-1 `99.9666%`, within +/-8 `99.9852%`, max abs diff
  `373`, mean abs diff `0.136`. All pixels with abs diff > 8 are in the final
  two raw rows; the inner image excluding a 32-pixel border is within +/-1
  everywhere. ACR DNG output is a useful reference, not a byte-parity target.
- Z8 `he.NEF` and `he_star.NEF` are unchanged from the good baselines.
- The Z9 HE fix is **not** a vertical-IDWT entry-state change. The effective
  changes are:
  - p16 GCLI reset for every structural overlap row.
  - Calibrated mixed-Bp GTLI rows `(3,9)`, `(3,10)`, `(4,8)`, `(4,9)`,
    `(4,10)`.
  - Low-Bp HE* GTLI corrections for `(2,0)`, `(2,1)`, `(2,3)`, `(2,4)`,
    `(2,7)`, `(2,8)`.
- The Z6 III follow-up confirms that HE/HE* are not camera-model-specific
  branches. The encoder dynamically chooses `Bp/Br` regimes per precinct;
  missing GTLI rows need exact lookup first, downward derivation from higher
  `Bp` when available, and controlled upward extrapolation when a sample
  selects a higher `Bp` than any explicit row at the same `Br`.
- The Z50 II follow-up shows that significance substream byte counts are
  per-line-block, derived from each LB's group count, not one global `f20`
  value for all eight LBs.

The sections below keep the investigation history. Any older "partial" or
"vertical carry" conclusion is superseded by the final 2026-06-21 update.

## Method

- Parse Nikon NEF raw SubIFD with Compression `34713`.
- Precinct stream starts at raw strip offset + `0x9b`.
- Per-precinct prefix fields used for variant identification:
  - bytes `0..2`: big-endian payload size
  - byte `3`: `Bp`
  - byte `4`: `Br`
- A 6-byte pad follows every 16th file precinct.
- Tile indexing used by the current decoder remains: tile `T` uses file
  precincts `T*16 .. T*16+17`, with overlap precincts shared with the next
  tile. Partial last tiles end early; for example full-frame height `5520`
  walks 1380 file precincts, not `ceil(5520/64)*16+2`.
- Validation dumps `LibRaw::open_file()` + `unpack()` raw planes and compares
  the full `raw_image` plane to the matching DNG raw plane with NumPy absolute
  differences.

## Format Notes

Nikon HE / HE* NEFs use a JPEG XS-like codestream embedded in the normal TIFF
raw SubIFD. The SubIFD still reports Nikon Compression `34713`, but the strip
data starts with a JPEG XS SOC/capability marker sequence `FF 10 FF 50`.
LibRaw routes the file to `nikon_he_load_raw()` by checking that marker at the
raw strip offset in the Nikon compression branch. Current validation coverage
is Z8 / Z9 / Z6 III full-frame samples plus a Z50 II DX sample; the marker, not
a fixed camera-model list, is the routing signal.

The visible camera menu names are not enough to identify the concrete entropy
regime. The decoder classifies the stream from precinct headers:

- Each precinct starts with a 12-byte prefix: 24-bit big-endian payload size,
  one-byte `Bp`, one-byte `Br`, then 28 two-bit `Dpb` fields.
- The prefix is followed by 8 line-block mini-headers and their interleaved
  significance, GCLI, coefficient-data, and sign substreams.
- A full-frame file has 1380 file precincts for 5520 raw rows. Every group of
  16 file precincts is followed by a 6-byte pad, and each 64-row tile decodes
  file precincts `T*16 .. T*16+17`, reusing two overlap precincts.

The observed full-frame variants are:

| Variant | Bp counts in sample | Observed Bp regime | Practical meaning |
|---|---:|---|---|
| Z8 normal HE | `Bp5=1379`, `Bp4=1` | mostly `Bp=5` | Baseline full-frame HE |
| Z8 HE* | `Bp4=1371`, `Bp3=9` | mostly `Bp=4`, rare `Bp=3` | Higher-Bp HE* overlap regime |
| Z9 normal HE | `Bp4=1175`, `Bp3=121`, `Bp5=84` | mixed `Bp=3/4/5` | Special full-frame HE |
| Z9 HE* | `Bp2=800`, `Bp3=550`, `Bp1=30` | `Bp=1/2/3` | Low-Bp special full-frame HE* |
| Z6 III normal HE | `Bp5=569`, `Bp4=210`, `Bp6=121`, `Bp3=110` | dynamic `Bp=3/4/5/6` | Dynamic high-Bp HE |
| Z6 III HE* | `Bp4=359`, `Bp2=316`, `Bp3=293`, `Bp5=42` | dynamic `Bp=2/3/4/5` | Dynamic HE* |
| Z50 II `DSC_0012.NEF` | `Bp2=811`, `Bp3=118`, `Bp4=3` | `Bp=2/3/4` | Validated DX low-Bp HE* |

The important discovery is that these are not separate opaque formats. They
share the same precinct syntax, GCLI entropy model, dequantization, wavelet
reconstruction, and Bayer merge. The main differences are the observed `Bp/Br`
regime, the needed GTLI rows, and the structural p16 GCLI predecessor reset.
GTLI rows follow the JPEG XS-style rule that one `Bp` step changes GTLI by one
level at the same `Br`: lower `Bp` rows derive downward from higher `Bp` rows
by subtracting and clamping, while Z6 III HE shows that higher `Bp` rows can
also be extrapolated upward from the nearest lower row when no higher row
exists.

Concrete HE/HE* names therefore mean "the camera selected one of several
quantization regimes", not "use one fixed decoder branch":

- Z8 `HE.NEF`: high-Bp HE, essentially all file precincts use `(Bp=5, Br=15..24)`.
- Z8 `he_star.NEF`: high-Bp HE*, mostly `(Bp=4, Br=11..24)` with a few low
  overlap rows at `Bp=3`.
- Z9 `HE.NEF`: mixed-Bp HE. It uses many `(Bp=4, Br=0..24)` precincts plus
  lower `Bp=3` and some `Bp=5` rows, so missing middle GTLI rows `(3,9)`,
  `(3,10)`, `(4,8)`, `(4,9)`, `(4,10)` were fatal.
- Z9 `HE_star.NEF`: low-Bp HE*. It uses `(Bp=1/2/3)` rows and only became
  stable after the low-Bp rows matched the downward GTLI derivation rule,
  including `(2,3)` sb9 and `(2,4)` sb14.
- Z6 III `HE.NEF`: dynamic high-Bp HE. It uses `Bp=6` and low-Br `Bp=5`
  precincts inside one normal HE file. It only became stable after p16 reset
  was tied to precinct position and missing high-Bp GTLI rows were extrapolated
  upward from same-Br lower-Bp rows.

The current evidence says `HE` and `HE*` are best treated as quality/bit-budget
families layered over the same codestream, with the actual format behavior
determined by the per-precinct `Bp/Br/Dpb` headers.

## Decoder Outline

The current implementation follows this pipeline:

1. Locate the JPEG XS-like raw strip payload at `strip_offset + 0x9b`, then walk
   precinct payloads using the 24-bit size field and 6-byte pad rule.
2. Build the full-frame sub-band layout for half-width processing. Each
   precinct has 26 sub-bands in 8 line blocks: pass-A wavelet horizontals,
   pass-A LL, pass-B wavelet horizontals, and pass-B LL.
3. For every tile, initialize a fresh GCLI predecessor state and decode 18
   precincts. Tiles advance by 16 file precincts, so the last two are shared
   with the next tile.
4. For each precinct, parse `Bp/Br/Dpb`, look up the 26-entry GTLI row, decode
   GCLI values from significance/unary streams, unpack bit-plane magnitudes,
   apply signs, and dequantize coefficients.
5. Reset the GCLI predecessor state at precinct index 16. This is a structural
   overlap position; the encoder may choose `Bp=6`, `Bp=5`, `Bp=4`, or low-Bp
   HE* regimes there, so the reset must not be tied to a fixed `Bp` allowlist.
6. Run horizontal 5/3 inverse DWT for pass A and pass B, feed the vertical 5/3
   lift state machine, preserve the two-stripe overflow between tiles, then run
   the two-stage Bayer merge into LibRaw's `raw_image` plane.

This decoder does not call Nikon NX Studio or any closed SDK. The validation
threshold is raw-plane agreement with Adobe DNG references before demosaic and
color transforms.

## Current Code State

The branch already contains the GTLI/routing work in commit `a59c2688`
(`nikon_he: add HE* support and fix HE band-scramble (missing gtli rows)`):

- Added missing full-frame HE `Bp=5, Br=17/18/19` GTLI rows.
- Added downward GTLI derivation from higher `Bp` at the same `Br`.
- Re-enabled HE* routing.
- Updated camera list/changelog wording.

Additional local source changes on top of that branch state:

- `src/decoders/nikon_he/nikon_he_predecessor.h`
- `should_reset_gcli()` now resets p16 for every structural overlap row. Z6 III
  HE showed that p16 can be `Bp == 6`, so the reset cannot be keyed by a fixed
  `Bp` allowlist.
- `src/decoders/nikon_he/nikon_he_gtli_table.cpp`
- Corrected low-Bp HE* rows `(2,0)`, `(2,1)`, `(2,3)`, `(2,4)`, `(2,7)`,
  `(2,8)`.
- Added and calibrated mixed-Bp HE rows `(3,9)`, `(3,10)`, `(4,8)`, `(4,9)`,
  `(4,10)`.
- Added controlled higher-Bp GTLI extrapolation from the nearest lower-Bp row
  at the same `Br`, used after exact and downward lookup fail. This covers
  Z6 III dynamic `Bp=6` and low-Br `Bp=5` HE precincts.
- `src/decoders/nikon_he/nikon_he_precinct_header.cpp`
- Significance substream byte counts are now derived per line block from the
  LB's actual group count. Z50 II DX exposed that lift LBs and LL/pass-B LBs
  can use different sig byte counts; a single global `f20` desynchronizes the
  mini-header walk.
- `src/metadata/tiff.cpp`
- HE routing now probes the JPEG XS-like `FF 10 FF 50` marker instead of a
  fixed camera-model allowlist.

The p16 reset change fixes the Z8 higher-Bp HE* boundary artifact, Z9 mixed-Bp
HE, and Z6 III dynamic high-Bp HE, where the same overlap precinct is decoded
both as a previous tile's p16 and the next tile's p0.

## Variant Evidence

The file names alone are not enough. The actual `Bp` regimes split into at least
four practical groups:

| File | Geometry | Observed `Bp` regime | Interpretation |
|---|---:|---|---|
| `he.NEF` | Z8 FF | mostly `Bp=5` | Normal full-frame HE |
| `he_star.NEF` | Z8 FF | mostly `Bp=4`, rare `Bp=3` | Higher-Bp full-frame HE* variant, not PR-style low-Bp |
| `Nikon - Z9-  HE_star.NEF` | Z9 FF | `Bp=1/2/3` | Low-Bp special full-frame HE* |
| `Nikon - Z9-  HE.NEF` | Z9 FF | mixed `Bp=3/4/5` | Special full-frame HE |
| `Nikon - Z6_3 - HE.NEF` | Z6 III FF | dynamic `Bp=3/4/5/6` | Dynamic high-Bp HE |
| `Nikon - Z6_3 - HE_star.NEF` | Z6 III FF | dynamic `Bp=2/3/4/5` | Dynamic HE* |
| `DSC_0012.NEF` | Z50 II DX | `Bp=2/3/4` | Validated low-Bp DX HE* |
| `Nikon - Z50_2 - DX HE_star.NEF` | Z50 II DX | `Bp=1/2/3` | Corrupt local sample |
| `Nikon - Z50_2 - DX HE.NEF` | Z50 II DX | mixed `Bp=3/4/5` | Corrupt local sample |

Important p16 reset observations:

- Z8 `he_star.NEF`: p16 overlap precincts are `Bp=4, Br=13/14`.
- Z9 `HE_star.NEF`: p16 overlap precincts are mostly `Bp=2, Br=11..18`.
- Z6 III `HE.NEF`: p16 overlap precincts can be `Bp=6`; this proves the reset
  must be structural (`precinct_index == 16`) rather than Bp-keyed.
- Z50 II `DSC_0012.NEF`: p16 overlap precincts are in the low-Bp `Bp=2/3/4`
  regime and follow the same structural reset rule.
- Z50 II DX `HE_star.NEF`: p16 overlap precincts are `Bp=2, Br=15/17/18`.
- Z9/Z50 special HE can have p16 in mixed `Bp=3/4/5` regimes.

## Historical Validation Snapshot

These were the full raw-plane comparisons before the final 2026-06-21
Z9 HE GTLI/reset fix. They are kept as a baseline history; see "Latest Status"
and the final update at the end for current results.

| Pair | Exact | Within +/-1 | Within +/-8 | Max abs diff | Status |
|---|---:|---:|---:|---:|---|
| Z8 `he.NEF` vs `he_dng.dng` | 86.8833% | 99.9955% | 99.9999% | 16 | Good |
| Z8 `he_star.NEF` vs `he_star_dng.dng` before p16 reset change | 74.3855% | 86.1307% | 86.6509% | 16383 | Boundary failure |
| Z8 `he_star.NEF` vs `he_star_dng.dng` after p16 reset change | 86.4527% | 99.9877% | 99.9984% | 38 | Good |
| Z9 `HE_star.NEF` vs `HE_star_dng.dng` | 67.5186% | 79.8343% | 86.2592% | 15849 | Partial, not solved |
| Z9 `HE.NEF` vs `HE_dng.dng` | 55.9869% | 65.2697% | 69.1065% | 16383 | Partial, not solved |
| Z50 II DX `HE_star.NEF` vs `HE_star_dng.dng` | 0.0015% | 0.0042% | 0.0245% | 16383 | Broken |
| Z50 II DX `HE.NEF` vs `HE_dng.dng` | 0.0014% | 0.0040% | 0.0229% | 16383 | Broken |

Z9 special notes:

- The Z9 special files do not look like total garbage. Their raw value
  distributions are close to the DNG references, but the error rates are much
  higher than the normal full-frame HE path.
- Z9 `HE_star.NEF`: mean abs diff about `17.4`, p99 `373`, p999 `1546`.
- Z9 `HE.NEF`: mean abs diff about `92.2`, p99 `1369`, p999 `9695`.
- Errors are not limited to rows `60..63` and `0..4`, although Z9 special HE
  still has stronger tile-boundary damage.

Z50 II DX notes:

- Both DX files decode into heavily clipped raw planes (`0` and `16383`
  dominate) with near-zero match to DNG.
- LibRaw emitted `data corrupted` warnings during unpack for both DX NEFs.
- This is not a simple crop, black-level, or Bayer-parity mismatch.

## Historical Limitation

This section describes the limitation before the 2026-06-21 Z9 HE / HE* GTLI
fixes.

At that point, the decoder handled:

- Normal full-frame HE (`Bp=5` dominant), after the missing GTLI rows.
- The Z8 higher-Bp HE* sample (`Bp=4` dominant) after extending the p16 GCLI
  reset rule to all `Bp=4` p16 rows.

At that point, the decoder did not correctly handle:

- Low-Bp special full-frame HE* (`Bp=1/2/3`) as seen in the Z9 sample.
- Mixed-Bp special full-frame HE as seen in the Z9 sample.
- DX HE / HE* special encodings as seen in the Z50 II samples.

The failing special encodings needed calibrated GTLI rows plus a broader p16
reset rule. Follow-up work showed the final low-Bp HE* residual also matched
the JPEG XS-style downward GTLI derivation rule from higher-Bp rows.

## Historical Suggested Next Steps

These were the next steps before the later 2026-06-21 follow-up resolved Z9,
Z6 III, and the valid Z50 II DX sample. They are kept as an investigation
trace, not as the current open-task list.

1. Keep variant classification explicit. Use actual `Bp/Br` histograms and
   image geometry, not file names, to route experiments.
2. Add or keep a raw-plane comparison harness. Validate before demosaic and
   color transforms.
3. Investigate Z9 special HE* first. It has full-frame geometry and partial
   numeric agreement, so it is the lowest-noise case for isolating the missing
   rule.
4. Investigate Z50 II DX separately. DX may need distinct sub-band layout,
   precinct/tile-tail handling, GTLI rows, or reset/orchestration rules.
5. Compare intermediate stages, not only final Bayer:
   - parsed `Bp/Br/Dpb`
   - GTLI per sub-band
   - GCLI prediction state around p16
   - post-entropy coefficient buffers
   - horizontal IDWT output
   - vertical IDWT/tile-overflow handoff
6. Be careful with local MSVC rebuilds. `nmake` has reused stale `.obj` files
   in this tree; force-delete touched `object/*.obj`, `bin/libraw.dll`, and
   `lib/libraw.lib` / `lib/libraw_static.lib` before trusting decoder results.
7. Avoid `dcraw_emu.exe` for deep validation unless rebuilt with a larger stack.
   Raw-plane dumps or a stack-raised test utility are safer for this decoder.
8. The later follow-up solved Z9 full-frame HE / HE*, Z6 III dynamic Bp/Br, and
   the valid Z50 II DX sample to raw-plane quantization tolerance. Future work
   should focus on more non-corrupt samples and keeping the GTLI table
   derivation explicit enough to avoid overfitting single files.

## 2026-06-20 Follow-up Investigation

A reproduceable raw-plane harness was rebuilt (`samples/he_dump.cpp` →
`bin/he_dump.exe`, linked with `/STACK:67108864` against `lib/libraw.lib`,
compared with `_cmp.py` / `_spatial.py` / `_tilemap.py` / `_he_star_tile.py`
in the LibRaw root). Baselines reproduce exactly:

| Pair | Exact | Max | Notes |
|---|---:|---:|---|
| Z8 `he.NEF` | 86.8833% | 16 | unchanged |
| Z8 `he_star.NEF` | 86.4527% | 38 | p16-reset result, unchanged |
| Z9 `HE.NEF` | 55.9869% | 16383 | unchanged |
| Z9 `HE_star.NEF` | 67.5186% | 15849 | unchanged |

### Z50 II DX — old corrupt files, later valid sample

`Nikon - Z50_2 - DX HE*.NEF` is **corrupt on disk** — Adobe Camera Raw
also fails to open it, and LibRaw's header parser walks into garbage at
LB1 of precinct 0 (gcli=495616, a 20-bit field maxed). The same exercise also
showed that model-name routing was too narrow for future bodies. The routing
path now keys off the JPEG XS-like `FF 10 FF 50` marker instead of an explicit
Z-body allowlist, so genuine future/Z50 II HE files can enter
`nikon_he_load_raw()`. The corrupt DX samples still fail, but that is the file,
not the decoder.

The later valid sample
`D:\Projects\pu-erh_lab\alcedo_studio\tests\resources\sample_images\raw\he_raw\Z50_2\DSC_0012.NEF`
removes the DX blocker. It exposed the real DX parsing issue: significance
substream byte counts are per-line-block, not one global `f20` value for all
eight LBs. After fixing that, the sample decodes to raw-plane quantization
tolerance against `DSC_0012_dng.dng`.

### Z9 special HE / HE* — vertical-IDWT carry-state hypothesis

Superseded by the final 2026-06-21 GTLI/reset result below. This section is
kept as investigation history.

Spatial error analysis (`_tilemap.py`, `_he_star_tile.py`) initially narrowed the
Z9 special failures to a **per-tile vertical-IDWT carry-state** bug, not
a GTLI-table gap:

- Damage is tile-local: it **resets at every tile boundary** and never
  spans more than one tile. Many tiles are *perfect* (mean 0.13, max 1)
  while neighboring tiles with identical `(Bp,Br)` regimes are wrecked.
  Identical Bp/Br sometimes OK, sometimes broken → not a GTLI lookup
  miss (that would break every tile of a given Bp/Br uniformly).
- Inside a broken tile the error is a **bell curve**: it grows linearly
  from the tile's start precinct, peaks mid-tile, and decays back toward
  the next tile boundary. This is the signature of a single wrong
  vertical-lift carry value (x2/x3) seeded at the tile entry, propagated
  through the 5/3 lifting chain, and naturally attenuating with distance.
- It is NOT concentrated in the precinct-16 overlap rows (60..63 / 0..4)
  the way the Z8 HE* boundary artifact was — it saturates the whole tile.

This points at the per-tile vertical-IDWT entry state in
`nikon_he_idwt_vertical.cpp` / `nikon_he_tile.cpp`: tiles after the first
enter at `VerLiftState::kInit0` (path-B entry), seeded from the previous
tile's `overflow_carry`. The Z9 special variants evidently need a
different entry state or a different carry seed than the Z8 FF path, OR
the p16 GCLI reset (`should_reset_gcli`) is firing/not-firing at the wrong
tile for the Z9 Bp=1/2/3 regime and desyncing the lift chain. The Z8
p16 reset rule (`Bp==4 && Br<=15`) is confirmed safe; the Z9 HE* p16
overlap precincts are `Bp=2, Br=11..18`, covered by the
`Bp==1||Bp==2||Bp==3` arm.

### 2026-06-21 Update — Z9 HE* low-Bp GTLI correction

The vertical-IDWT carry hypothesis above was useful for narrowing the
failure shape, but follow-up experiments showed the main Z9 HE* problem was
not the entry-state path. The bad HE* tiles were keyed by the 2-precinct
overlap row's low-Bp GTLI values:

- `(Bp=2, Br=0)` tiles 14/15/17 were repaired by lowering sb3 and sb5 by 1.
- `(Bp=2, Br=8)` tile 18 was repaired by sb4 +1, sb10 -1, sb17 +1.
- `(Bp=2, Br=1)` tile 21 was repaired by sb3 -1, sb5 -1, sb14 +1.

These changes were applied to `src/decoders/nikon_he/nikon_he_gtli_table.cpp`.
The raw-plane validation results after rebuilding `libraw.dll` and
`bin/he_dump.exe`:

| Pair | Exact | Within +/-1 | Within +/-8 | Max abs diff | Mean abs diff |
|---|---:|---:|---:|---:|---:|
| Z9 `HE_star.NEF` vs `HE_star_dng.dng` | 77.4469% | 91.8707% | 97.1450% | 8406 | 1.238 |
| Z8 `he_star.NEF` vs `he_star_dng.dng` | 86.4527% | 99.9877% | 99.9984% | 38 | 0.136 |
| Z8 `he.NEF` vs `he_dng.dng` | 86.8833% | 99.9955% | 99.9999% | 16 | 0.131 |

The previous Z9 HE* baseline was exact `67.5186%`, within +/-8 `86.2592%`,
max `15849`, mean about `17.4`, so the low-Bp GTLI correction fixes the
dominant HE* failure without regressing the Z8 HE/HE* samples.

Residual Z9 HE* tiles with mean abs diff > 5 remain: 25, 38, 39, 40, 41,
42, 50, 79, 81. Tile 25 did not respond to single-subband edits of
`(Bp=3, Br=21)`, so the remaining residual is likely not the same simple
GTLI-row issue.

Z9 `HE.NEF` is unchanged by these low-Bp HE* corrections:
exact `55.9869%`, within +/-8 `69.1065%`, max `16383`, mean `92.222`.
The Python port does not exactly match C++ on Z9 HE tile metrics, so use the
C++ raw-plane dump path for HE follow-up decisions.

### 2026-06-21 Update — Z9 HE missing middle GTLI rows

Z9 `HE.NEF` uses `(Bp,Br)` combinations absent from the table. Before this
pass, `lookup_gtli_table()` returned `nullptr` for `(Bp=3, Br=9/10)` and
`(Bp=4, Br=8/9/10)`, which the caller treated as all-zero GTLI. Those rows
occur in the Z9 HE precinct stream and are not safe to decode as zero.

Conservative intermediate rows were added:

- `(3,9)` and `(3,10)` between existing `(3,8)` and `(3,11)`.
- `(4,8)`, `(4,9)`, `(4,10)` between existing `(4,7)` and `(4,11)`.

After rebuilding and dumping `Nikon - Z9-  HE.NEF`, the raw-plane comparison
improved but is still far from solved:

| Pair | Exact | Within +/-1 | Within +/-8 | Max abs diff | Mean abs diff |
|---|---:|---:|---:|---:|---:|
| Z9 `HE.NEF` before middle rows | 55.9869% | 65.2697% | 69.1065% | 16383 | 92.222 |
| Z9 `HE.NEF` after middle rows | 57.1201% | 67.3748% | 74.0463% | 16383 | 72.115 |

Z9 HE* output was byte-identical before/after the middle-row additions, so
these rows only affect the mixed-Bp HE path.

Residual Z9 HE observations after the middle-row additions:

- All observed Z9 HE `(Bp,Br)` pairs now resolve through an exact or derived
  GTLI row; there are no remaining all-zero missing-row lookups for this
  sample.
- Many bad tiles are boundary-local, with the first or last four Bayer rows
  carrying most of the error; others show whole-tile drift. Good and bad
  tiles can alternate inside the same `(Bp=4, Br=11..24)` regime.
- This again points to tile vertical reconstruction / overlap handling for
  the mixed-Bp HE variant, but only after the GTLI table is complete enough
  not to confound the analysis.

### 2026-06-21 Update — Z9 HE mixed-Bp fix

Follow-up C++ raw-plane experiments showed that Z9 `HE.NEF` was not blocked on
vertical-IDWT entry state. The remaining failure was the combination of p16
GCLI predecessor reset and inaccurate conservative middle GTLI rows.

Production changes now applied:

- `should_reset_gcli()` resets p16 for every `Bp == 4` overlap row. This is
  required because the same file precinct is decoded as previous tile p16 and
  next tile p0; using a stale predecessor state on one side creates boundary
  explosions. It does not change the Z8 HE/HE* good baselines.
- Calibrated Z9 mixed-Bp HE GTLI rows:
  - `(3,9)`: sb2 +1, sb4 +1 versus the earlier conservative row.
  - `(3,10)`: sb4 +1.
  - `(4,8)`: sb2 +1, sb4 +1, sb17 +1.
  - `(4,9)`: sb2 +1, sb4 +1.
  - `(4,10)`: sb4 +1.
- Calibrated additional Z9 low-Bp HE* GTLI rows:
  - `(2,3)`: sb14 +1. This improves the overall Z9 HE* error tail and does
    not affect the Z9 HE or Z8 HE/HE* samples.
  - `(2,7)`: sb17 +1. This repaired most remaining HE* residual tiles
    without changing the Z8 HE/HE* baselines.

Final no-override raw-plane validation after rebuilding `libraw.dll` and
`bin/he_dump.exe`:

| Pair | Exact | Within +/-1 | Within +/-8 | Max abs diff | Mean abs diff |
|---|---:|---:|---:|---:|---:|
| Z9 `HE.NEF` vs `HE_dng.dng` | 86.6909% | 99.9927% | 99.9992% | 46 | 0.133 |
| Z9 `HE_star.NEF` vs `HE_star_dng.dng` | 85.0660% | 98.4610% | 99.4202% | 8951 | 0.350 |
| Z8 `he.NEF` vs `he_dng.dng` | 86.8833% | 99.9955% | 99.9999% | 16 | 0.131 |
| Z8 `he_star.NEF` vs `he_star_dng.dng` | 86.4527% | 99.9877% | 99.9984% | 38 | 0.136 |

The `(2,3)` sb14 +1 change worsens one tile-38 single-pixel spike from `8406`
to `8951`, but improves every aggregate Z9 HE* metric and cuts the high-error
tail substantially. For example, pixels with abs diff >512 drop from `790` to
`397`, and pixels with abs diff >1024 drop from `179` to `116` compared with
the `(2,7)`-only result. The remaining Z9 HE* residual tiles are 38, 40, 59,
and 79; most other previously bad tiles are back to quantization-level
differences.

### 2026-06-21 Final Update — Z9 HE* residual GTLI fix

The last Z9 low-Bp HE* residual was not a vertical-IDWT or tile-overlap state
bug. Comparing the remaining rows against the closest higher-Bp same-Br rows
showed two low-Bp entries still deviated from the JPEG XS-style downward GTLI
derivation rule:

- `(Bp=2, Br=3)`: sb9 lowered from `2` to `1`.
- `(Bp=2, Br=4)`: sb14 raised from `0` to `1`.

After applying those two GTLI corrections and rebuilding `libraw.dll`, the
four remaining HE* residual tiles 38, 40, 59, and 79 all dropped to
quantization-level differences.

Final no-override raw-plane validation after this pass:

| Pair | Exact | Within +/-1 | Within +/-8 | Max abs diff | Mean abs diff |
|---|---:|---:|---:|---:|---:|
| Z9 `HE_star.NEF` vs `HE_star_dng.dng` | 86.7453% | 99.9920% | 99.9990% | 47 | 0.133 |
| Z9 `HE.NEF` vs `HE_dng.dng` | 86.6909% | 99.9927% | 99.9992% | 46 | 0.133 |
| Z8 `he.NEF` vs `he_dng.dng` | 86.8833% | 99.9955% | 99.9999% | 16 | 0.131 |
| Z8 `he_star.NEF` vs `he_star_dng.dng` | 86.4527% | 99.9877% | 99.9984% | 38 | 0.136 |

Full LibRaw TIFF pipeline validation was also run with `bin/simple_dcraw.exe
-T -6` into
`D:\Projects\pu-erh_lab\alcedo_studio\tests\resources\sample_images\raw\he_raw`:

| Input | TIFF output | Result |
|---|---|---|
| `he.NEF` | `he.NEF.tiff` | Success |
| `he_star.NEF` | `he_star.NEF.tiff` | Success |
| `Nikon - Z9-  HE.NEF` | `Nikon - Z9-  HE.NEF.tiff` | Success |
| `Nikon - Z9-  HE_star.NEF` | `Nikon - Z9-  HE_star.NEF.tiff` | Success |
| `Z50_2\DSC_0012.NEF` | `Z50_2\DSC_0012.NEF.tiff` | Success |
| `Nikon - Z50_2 - DX HE.NEF` | none | Older corrupt local sample |
| `Nikon - Z50_2 - DX HE_star.NEF` | none | Older corrupt local sample |

There are no debug `NIKON_HE_*` environment-variable hooks or dump hooks left
in `src/decoders/nikon_he` or `samples/he_dump.cpp`. Continue to use C++
raw-plane dumps for future HE* follow-up; the Python port was useful for header
analysis but did not match the C++ decoder closely enough for final HE
decisions.

### 2026-06-21 Update — Z6 III dynamic Bp/Br robustness

The Z6 III samples in
`D:\Projects\pu-erh_lab\alcedo_studio\tests\resources\sample_images\raw\he_raw\Z6_3`
confirmed that HE/HE* are dynamically selected `Bp/Br` regimes inside one
codestream, not fixed camera-model or menu-label branches.

Observed Z6 III file geometry is `6064 x 4040` raw pixels. Both NEFs route to
`nikon_he_load_raw()` and have matching Adobe DNG references.

Observed `Bp` distributions:

| File | Observed `Bp` counts | Meaning |
|---|---:|---|
| `Nikon - Z6_3 - HE.NEF` | `Bp5=569`, `Bp4=210`, `Bp6=121`, `Bp3=110` | Dynamic high-Bp HE |
| `Nikon - Z6_3 - HE_star.NEF` | `Bp4=359`, `Bp2=316`, `Bp3=293`, `Bp5=42` | Dynamic HE* |

The first Z6 III HE run exposed two missing general rules:

- `Bp=6` and low-Br `Bp=5` rows must not fall back to all-zero GTLI. When no
  higher-Bp row exists at the same `Br`, the decoder now extrapolates upward
  from the nearest lower-Bp row by adding one GTLI level per `Bp` step. Exact
  rows still win, and downward derivation from a higher row is still preferred.
- p16 GCLI reset is structural. Z6 III HE has p16 overlap precincts with
  `Bp=6`, so `should_reset_gcli()` now resets every precinct index 16 instead
  of matching a fixed `Bp` allowlist.

Z6 III HE raw-plane progression:

| State | Exact | Within +/-1 | Within +/-8 | Max abs diff | Mean abs diff |
|---|---:|---:|---:|---:|---:|
| Before high-Bp GTLI extrapolation | 44.7763% | 51.8350% | 52.0295% | 16383 | 506.209 |
| After high-Bp GTLI extrapolation only | 82.7702% | 95.5584% | 95.6612% | 16383 | 106.016 |
| After structural p16 reset | 86.6352% | 99.9936% | 99.9999% | 18 | 0.134 |

Final Z6 III raw-plane validation:

| Pair | Exact | Within +/-1 | Within +/-8 | Max abs diff | Mean abs diff |
|---|---:|---:|---:|---:|---:|
| Z6 III `HE.NEF` vs `HE_dng.dng` | 86.6352% | 99.9936% | 99.9999% | 18 | 0.134 |
| Z6 III `HE_star.NEF` vs `HE_star_dng.dng` | 86.6356% | 99.9930% | 99.9996% | 35 | 0.134 |

Z8/Z9 regression after these changes is unchanged:

| Pair | Exact | Within +/-1 | Within +/-8 | Max abs diff | Mean abs diff |
|---|---:|---:|---:|---:|---:|
| Z8 `he.NEF` vs `he_dng.dng` | 86.8833% | 99.9955% | 99.9999% | 16 | 0.131 |
| Z8 `he_star.NEF` vs `he_star_dng.dng` | 86.4527% | 99.9877% | 99.9984% | 38 | 0.136 |
| Z9 `HE.NEF` vs `HE_dng.dng` | 86.6909% | 99.9927% | 99.9992% | 46 | 0.133 |
| Z9 `HE_star.NEF` vs `HE_star_dng.dng` | 86.7453% | 99.9920% | 99.9990% | 47 | 0.133 |

Full TIFF pipeline validation with `bin/simple_dcraw.exe -T -6` also succeeded:

| Input | TIFF output | Time | Result |
|---|---|---:|---|
| `Nikon - Z6_3 - HE.NEF` | `Nikon - Z6_3 - HE.NEF.tiff` | 8.91s | Success |
| `Nikon - Z6_3 - HE_star.NEF` | `Nikon - Z6_3 - HE_star.NEF.tiff` | 8.57s | Success |

Local raw-plane dump timings were `2.80s` for Z6 III HE and `3.02s` for Z6 III
HE*. The Z6 III NEF compression ratios against a `6064 x 4040 x uint16` raw
plane are about `3.65:1` for HE and `2.51:1` for HE*.
