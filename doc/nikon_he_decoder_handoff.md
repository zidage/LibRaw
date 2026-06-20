# Nikon HE / HE* Decoder Handoff

Date: 2026-06-20
Branch: `codex/merge-pr-825-826`

This note records the current evidence for Nikon HE / HE* decoding work in this
LibRaw checkout. The findings are based on raw-plane comparisons against Adobe
DNG reference output, not visual inspection.

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

## Current Code State

The branch already contains the GTLI/routing work in commit `a59c2688`
(`nikon_he: add HE* support and fix HE band-scramble (missing gtli rows)`):

- Added missing full-frame HE `Bp=5, Br=17/18/19` GTLI rows.
- Added downward GTLI derivation from higher `Bp` at the same `Br`.
- Re-enabled HE* routing.
- Updated camera list/changelog wording.

There is one additional local source change:

- `src/decoders/nikon_he/nikon_he_predecessor.h`
- `should_reset_gcli()` now resets p16 for `Bp == 4 && Br <= 15`, not only
  `Br <= 7`.

This reset change fixes the Z8 higher-Bp HE* boundary artifact without changing
the normal Z8 HE result.

## Variant Evidence

The file names alone are not enough. The actual `Bp` regimes split into at least
four practical groups:

| File | Geometry | Observed `Bp` regime | Interpretation |
|---|---:|---|---|
| `he.NEF` | Z8 FF | mostly `Bp=5` | Normal full-frame HE |
| `he_star.NEF` | Z8 FF | mostly `Bp=4`, rare `Bp=3` | Higher-Bp full-frame HE* variant, not PR-style low-Bp |
| `Nikon - Z9-  HE_star.NEF` | Z9 FF | `Bp=1/2/3` | Low-Bp special full-frame HE* |
| `Nikon - Z9-  HE.NEF` | Z9 FF | mixed `Bp=3/4/5` | Special full-frame HE |
| `Nikon - Z50_2 - DX HE_star.NEF` | Z50 II DX | `Bp=1/2/3` | Low-Bp special DX HE* |
| `Nikon - Z50_2 - DX HE.NEF` | Z50 II DX | mixed `Bp=3/4/5` | Special DX HE |

Important p16 reset observations:

- Z8 `he_star.NEF`: p16 overlap precincts are `Bp=4, Br=13/14`.
- Z9 `HE_star.NEF`: p16 overlap precincts are mostly `Bp=2, Br=11..18`.
- Z50 II DX `HE_star.NEF`: p16 overlap precincts are `Bp=2, Br=15/17/18`.
- Z9/Z50 special HE can have p16 in mixed `Bp=3/4/5` regimes.

## Current Validation Results

Metrics are full raw-plane comparisons against the listed DNG references.

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

## Current Limitation

The current decoder handles:

- Normal full-frame HE (`Bp=5` dominant), after the missing GTLI rows.
- The Z8 higher-Bp HE* sample (`Bp=4` dominant) after extending the p16 GCLI
  reset rule to `Bp=4 && Br <= 15`.

The current decoder does not correctly handle:

- Low-Bp special full-frame HE* (`Bp=1/2/3`) as seen in the Z9 sample.
- Mixed-Bp special full-frame HE as seen in the Z9 sample.
- DX HE / HE* special encodings as seen in the Z50 II samples.

The failing special encodings likely need more than generic GTLI downward
derivation and the current p16 reset rule. The next investigation should treat
them as separate codec/layout variants, not as ordinary HE/HE* rows missing
from the same table.

## Suggested Next Steps

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

