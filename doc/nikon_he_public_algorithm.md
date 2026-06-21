# How to Read Nikon's HE and HE* RAW Files Without Nikon's SDK

A field guide to a JPEG XS-like RAW codestream hiding inside Nikon NEF files.

Date: 2026-06-21

There is a kind of Nikon RAW file that most open-source software has treated
as a locked room. You can make the camera write it. Nikon's own software can
open it. Adobe can turn it into a DNG. But if you try to follow the bytes
yourself, you quickly stop seeing anything that looks like ordinary Bayer data.

This article is about opening that room.

The short version is that the current LibRaw branch can decode the available
Nikon Z8, Z9, Z6 III, and Z50 II HE / HE* NEF samples without calling Nikon NX
Studio, without using Nikon's closed SDK, and without treating Adobe's output as
a magic black box. The decoded raw planes land inside the expected
quantization-error range against Adobe DNG references.

That is not the same as claiming that every Nikon HE / HE* file on Earth is now
solved. It is not Adobe Camera Raw bit-exact, and byte parity is not a sensible
goal because ACR's DNG output may itself be compressed or otherwise processed.
Older corrupt Z50 II DX samples in this test set still cannot prove anything
useful, but a later valid Z50 II DX sample now does decode correctly. Future
cameras may expose more rows in the rate-control table. But the important thing
has changed: Nikon HE / HE* is no longer just "the thing the SDK knows how to
do." It is a readable JPEG XS-like wavelet bit-plane codestream.

## "Wait, what is being compressed here?"

If you have only met RAW files through photo editors, it is easy to think of
RAW as a kind of image. It is more useful to think of it as a sensor record.

A Nikon NEF is a TIFF-like container. Inside it is a Bayer mosaic, meaning that
each photosite records one color component through the camera's color filter
array. A normal photo has already been demosaiced, white-balanced, color
transformed, tone-mapped, sharpened, and usually compressed for display. A RAW
file is much closer to the measuring instrument.

That is why lossy RAW compression sounds suspicious. If RAW is the thing you
use when you want maximum editing latitude, why would anyone throw information
away before the edit even starts?

The answer is bandwidth. A Z8 / Z9 full-frame raw plane here is:

```text
8280 * 5520 * 2 = 91,411,200 bytes
```

That is before previews, metadata, and file container overhead. A camera that
shoots fast has to move this data through sensor readout, image processing,
memory, and storage. Nikon HE and HE* are a practical bargain: keep data in the
RAW domain, discard a controlled amount of precision, and write much smaller
files.

The files in this sample set compress the 16-bit raw plane by roughly 2.5:1 to
4.4:1. That is why the format exists.

## "Isn't this just JPEG?"

No, but ordinary JPEG gives the right first intuition.

Classic JPEG divides an image into 8x8 blocks, transforms each block with a
DCT, quantizes the transformed coefficients, and entropy-codes the result.
There are two big ideas there:

1. Change the representation so the important information is concentrated.
2. Store the less important precision more cheaply, or not at all.

Nikon HE / HE* follows the same broad philosophy, but not the same machinery.
It is much closer to JPEG XS than to old 8x8 JPEG.

JPEG XS is built for high-throughput, low-latency, visually near-lossless
coding. A typical JPEG XS pipeline uses wavelet transforms, bit-plane coding,
small spatial units, and explicit truncation information. Those are exactly the
ideas that show up in Nikon HE / HE*.

Two JPEG XS terms are especially helpful:

- GCLI means Group Code Length Information. It tells you how high a group of
  coefficients reaches in bit-plane terms.
- GTLI means Greatest Truncation Level Information. It tells you how many low
  bit-planes were thrown away for a sub-band.

If GCLI says "how tall is the number?", GTLI says "how much of the bottom did
the encoder cut off?"

That is the mental model that makes the Nikon stream become legible.

## The NEF is only the wrapper

Nikon HE / HE* files are still NEF files. The raw SubIFD reports Nikon's private
compression value:

```text
34713
```

The actual compressed stream lives in the raw strip. The strip begins with a
small header, and the precinct stream starts at:

```text
strip_offset + 0x9b
```

At the raw strip offset itself, the files begin with a JPEG XS-like marker:

```text
FF 10 FF 50
```

LibRaw now uses that marker in the Nikon compression branch to route the file to
`nikon_he_load_raw()`. The currently validated cameras are Z8, Z9, Z6 III, and
Z50 II samples, but the marker is the important routing signal. It is not a hard
camera-model allowlist.

The implementation lives mostly here:

```text
src/decoders/nikon_he/
```

The most important files are:

| File | Role |
|---|---|
| `nikon_he_decode.cpp` | Image-level tile and precinct scheduling |
| `nikon_he_precinct_decode.cpp` | Entropy decoding for one precinct |
| `nikon_he_gtli_table.cpp` | `(Bp, Br) -> 26 GTLI values`, plus derivation |
| `nikon_he_predecessor.h` | GCLI predecessor state and p16 reset |
| `nikon_he_idwt_horizontal.cpp` | Horizontal 5/3 inverse wavelet transform |
| `nikon_he_idwt_vertical.cpp` | Vertical 5/3 inverse wavelet transform |
| `nikon_he_bayer.cpp` | Merge the reconstructed wavelet result into Bayer |

## A field guide to the precinct

If classic JPEG has 8x8 blocks, Nikon HE / HE* has precincts.

Every precinct starts with a 12-byte header:

| Field | Size | Meaning |
|---|---:|---|
| `size` | 24-bit big-endian | Payload size after the 12-byte header |
| `Bp` | 8-bit | Bit-plane / precision regime |
| `Br` | 8-bit | Rate / refinement regime |
| `Dpb` | 28 packed 2-bit fields | Extra per-line-block depth hints |

The payload then contains eight line blocks. Each line block contains small
headers and interleaved substreams for:

- significance and unary information used to decode GCLI,
- GCLI values,
- coefficient magnitude bit-planes,
- sign bits.

The Z8 / Z9 full-frame samples have a raw plane of:

```text
8280 x 5520
```

They contain 1380 file precincts. After each group of 16 file precincts, there
is a 6-byte pad. Tiles are 64 raw rows high, but a tile does not decode just 16
precincts. It decodes 18:

```text
tile T uses file precincts T*16 .. T*16+17
```

That means neighboring tiles share two overlap precincts. A lot of the format's
sharp edges live in that overlap. If you decode the bytes but mishandle the
state that crosses this boundary, the image can look as if the wavelet
reconstruction itself is broken.

## HE and HE* are not two fixed formats

The camera menu makes the world look simple:

```text
HE  = one format
HE* = another format
```

That is not what the bytes show.

The better description is that HE and HE* are quality or bit-budget families
inside one codestream syntax. The actual decoding behavior is selected per
precinct by the header fields:

```text
Bp / Br / Dpb
```

The Z6 III samples made this impossible to ignore. A single ordinary HE file
uses `Bp=3/4/5/6` inside one image. That is not a Z6 III-specific alternate
format. It is the encoder dynamically choosing bit budgets per precinct.

Here are the observed full-frame `Bp` distributions:

| Sample | Menu name | Observed Bp counts | Practical reading |
|---|---|---:|---|
| Z8 `he.NEF` | HE | `Bp5=1379`, `Bp4=1` | High-Bp ordinary HE |
| Z8 `he_star.NEF` | HE* | `Bp4=1371`, `Bp3=9` | High-Bp HE*, mostly `Bp=4` |
| Z9 `Nikon - Z9-  HE.NEF` | HE | `Bp4=1175`, `Bp3=121`, `Bp5=84` | Mixed-Bp HE |
| Z9 `Nikon - Z9-  HE_star.NEF` | HE* | `Bp2=800`, `Bp3=550`, `Bp1=30` | Low-Bp HE* |
| Z6 III `Nikon - Z6_3 - HE.NEF` | HE | `Bp5=569`, `Bp4=210`, `Bp6=121`, `Bp3=110` | Dynamic high-Bp HE |
| Z6 III `Nikon - Z6_3 - HE_star.NEF` | HE* | `Bp4=359`, `Bp2=316`, `Bp3=293`, `Bp5=42` | Dynamic HE* |
| Z50 II `DSC_0012.NEF` | DX HE* | `Bp2=811`, `Bp3=118`, `Bp4=3` | Validated DX low-Bp HE* |

This table is the key clue. Z8 HE* and Z9 HE* do not even live in the same Bp
regime. Z9 HE is not a simple copy of Z8 HE. Z6 III HE proves that a single
file can move between several regimes as the encoder sees fit.

So the decoder should not ask "is this HE or HE*?" and then jump to a separate
branch. It should do this:

1. Read the unified precinct syntax.
2. Use each precinct's `Bp` and `Br` to choose GTLI values.
3. Run the same GCLI, bit-plane, dequantization, IDWT, and Bayer merge pipeline.

Once you look at it that way, the format becomes much less mysterious.

## The little table that unlocks the stream

Every precinct has 26 sub-bands. The current implementation uses this order:

| Sub-band range | Contents |
|---|---|
| `sb0..5` | Pass A deeper horizontal wavelet bands |
| `sb6..11` | Pass A next horizontal wavelet bands |
| `sb12` | Pass A LL band |
| `sb13..18` | Pass A final horizontal wavelet bands |
| `sb19..20` | Pass B first horizontal wavelet bands |
| `sb21..22` | Pass B next horizontal wavelet bands |
| `sb23` | Pass B LL band |
| `sb24..25` | Pass B final horizontal wavelet bands |

Before decoding a sub-band, the decoder needs its GTLI. The table is:

```text
(Bp, Br) -> 26 GTLI values
```

If a precinct says `Bp=4, Br=9`, the decoder looks up row `(4,9)` and receives
26 small integers. Each integer tells one sub-band how many low bit-planes were
truncated.

At first, many failures looked unrelated. Some were tile boundary explosions.
Some were whole-tile drift. Some were narrow residuals in only a few places.
But several of them came back to the same thing: the decoder did not yet know
the right GTLI row.

The first rule discovered was the downward rule:

```text
At the same Br, lowering Bp by 1 lowers GTLI by 1, clamped at 0.
```

That is exactly the sort of rule a JPEG XS-like bit-plane codec would have.
Lower Bp means less retained precision. The low-Bp Z9 HE* residual finally
collapsed when the table was made consistent with this rule, including:

```text
(Bp=2, Br=3): sb9  2 -> 1
(Bp=2, Br=4): sb14 0 -> 1
```

Then the Z6 III added the missing half of the story. It uses `Bp=6` in normal
HE, and it also uses low-Br `Bp=5` rows that were not present in the original
Z8 / Z9 table. If no higher-Bp row exists at the same `Br`, the decoder now
uses the closest lower-Bp row and extrapolates upward:

```text
At the same Br, raising Bp by 1 raises GTLI by 1.
```

The lookup priority matters:

1. Exact table row.
2. Downward derivation from the nearest higher `Bp` at the same `Br`.
3. Controlled upward extrapolation from the nearest lower `Bp` at the same
   `Br`.

Upward extrapolation is deliberately last. A low-Bp row may have already hit
the `0` clamp, and once a value is clamped you cannot know exactly what it was
before the clamp. Exact rows and downward derivation preserve more information.

## The p16 reset, or why the borders exploded

GCLI is not decoded in total isolation. It has predecessor state. That state
helps compression, but it creates a second problem: when should the decoder
forget it?

In Nikon HE / HE*, tile-local precinct index 16 is a structural LL overlap
precinct. It can appear under different `Bp` regimes:

- ordinary full-frame HE may use `Bp=5`,
- mixed HE and high-Bp HE* may use `Bp=4`,
- low-Bp HE* may use `Bp=1/2/3`,
- Z6 III dynamic HE can use `Bp=6`.

The final rule is simple:

```text
if precinct_index == 16:
    reset the GCLI predecessor state
```

The important part is what the rule is not. It is not a list of Bp values.

Early versions reset only for a Bp-shaped guess at the LL band. That was enough
to pass some samples and fail others. Z6 III made the error obvious: p16 can be
`Bp=6`. The reset is tied to the structure of the tile overlap, not to a
specific precision regime selected by the encoder.

This was one of the best lessons in the investigation. A spatial error map can
tell you where the image is going wrong, but not always which module is guilty.
A GCLI predecessor mistake can masquerade as an inverse-wavelet carry problem.

## The whole decoder in one pass

Here is the current pipeline, stripped down to its essentials:

1. Parse the NEF/TIFF container and find the raw SubIFD.
2. Confirm Nikon compression `34713` and the JPEG XS-like `FF 10 FF 50` marker.
3. Start the precinct walk at `strip_offset + 0x9b`.
4. For each precinct, read the 24-bit size, `Bp`, `Br`, and `Dpb`.
5. Skip the 6-byte pad after every 16 file precincts.
6. Build 64-row tiles with 18 precincts each, advancing by 16 precincts.
7. Initialize tile-local GCLI predecessor state.
8. For each precinct, resolve the 26 GTLI values from `(Bp, Br)`.
9. Decode significance, unary coding, and GCLI.
10. Decode coefficient magnitude bit-planes and sign bits.
11. Dequantize into signed wavelet coefficients.
12. Run the horizontal 5/3 inverse DWT.
13. Run the vertical 5/3 inverse DWT, carrying the two-stripe overflow between
    tiles.
14. Merge the reconstructed data into LibRaw's Bayer `raw_image` plane.

The one-line map is:

```text
NEF/TIFF
  -> JPEG XS-like precinct stream
  -> GCLI/GTLI
  -> bit-plane coefficients
  -> 5/3 inverse wavelet transform
  -> Bayer raw plane
```

## How the discovery unfolded

The first useful sample was Z8 HE. It is mostly `Bp=5`, so it is the friendly
case. Once the precinct walk, bitstream unpacking, dequantization, wavelet
direction, and Bayer merge were basically right, the raw plane was already
close.

Z8 HE* broke at tile boundaries. The stream was not globally wrong. It was
wrong where state crossed structural boundaries. That led to the first version
of the p16 GCLI predecessor reset.

Z9 HE and HE* were more deceptive. Large parts of the image were already near
the Adobe DNG reference, which meant the overall architecture was right. The
bad regions initially looked like vertical IDWT carry-state bugs. That was a
good hypothesis, but it was not the final answer.

Z9 HE needed missing middle rows in the GTLI table:

```text
(3,9), (3,10), (4,8), (4,9), (4,10)
```

Z9 HE* needed the low-Bp rows to obey the downward GTLI rule. Once the last two
inconsistent entries were corrected, the remaining residual tiles fell back
into quantization-level differences.

Then Z6 III HE forced the model to become more general. It showed `Bp=6` inside
a normal HE file, and it showed that p16 reset cannot be keyed by Bp. The
progression was very sharp:

| Z6 III HE state | Exact | Within +/-1 | Within +/-8 | Max | Mean |
|---|---:|---:|---:|---:|---:|
| Before high-Bp GTLI extrapolation | 44.7763% | 51.8350% | 52.0295% | 16383 | 506.209 |
| After high-Bp extrapolation only | 82.7702% | 95.5584% | 95.6612% | 16383 | 106.016 |
| After structural p16 reset | 86.6352% | 99.9936% | 99.9999% | 18 | 0.134 |

That last jump is the moment the format stopped looking camera-specific.

## How do we know it works?

Looking at a TIFF is not enough. Demosaic, white balance, color matrices, tone
curves, clipping, and sharpening can hide raw-level errors. The reliable test is
to compare the Bayer plane before those steps.

The validation method is:

1. Use LibRaw `open_file() + unpack()` to produce `raw_image`.
2. Use Adobe DNG references to produce the corresponding raw plane.
3. Compare unsigned 16-bit samples pixel by pixel.

The current raw-plane results are:

| Sample | Exact | `abs(diff)<=1` | `abs(diff)<=8` | Max abs diff | Mean abs diff |
|---|---:|---:|---:|---:|---:|
| Z8 `he.NEF` | 86.8833% | 99.9955% | 99.9999% | 16 | 0.131 |
| Z8 `he_star.NEF` | 86.4527% | 99.9877% | 99.9984% | 38 | 0.136 |
| Z9 `Nikon - Z9-  HE.NEF` | 86.6909% | 99.9927% | 99.9992% | 46 | 0.133 |
| Z9 `Nikon - Z9-  HE_star.NEF` | 86.7453% | 99.9920% | 99.9990% | 47 | 0.133 |
| Z6 III `Nikon - Z6_3 - HE.NEF` | 86.6352% | 99.9936% | 99.9999% | 18 | 0.134 |
| Z6 III `Nikon - Z6_3 - HE_star.NEF` | 86.6356% | 99.9930% | 99.9996% | 35 | 0.134 |
| Z50 II `DSC_0012.NEF` | 86.8465% | 99.9666% | 99.9852% | 373 | 0.136 |

These are quantization-scale errors. They are not bit-exact to Adobe, but for a
lossy RAW format, and with Adobe itself not being a ground-truth sensor dump,
this is the evidence that the core decoding path is correct. In the Z50 II
sample, the only `abs(diff)>8` pixels are in the last two raw rows; excluding a
32-pixel border gives `abs(diff)<=1` for the entire inner image. That makes the
remaining tail much more likely to be reference-DNG compression or edge handling
than a structural decode error.

The full LibRaw TIFF pipeline also runs:

```powershell
bin\simple_dcraw.exe -T -6 <input.NEF>
```

Generated TIFFs in the sample tree:

| Input | Output TIFF |
|---|---|
| `he.NEF` | `he.NEF.tiff` |
| `he_star.NEF` | `he_star.NEF.tiff` |
| `Nikon - Z9-  HE.NEF` | `Nikon - Z9-  HE.NEF.tiff` |
| `Nikon - Z9-  HE_star.NEF` | `Nikon - Z9-  HE_star.NEF.tiff` |
| `Z6_3\Nikon - Z6_3 - HE.NEF` | `Z6_3\Nikon - Z6_3 - HE.NEF.tiff` |
| `Z6_3\Nikon - Z6_3 - HE_star.NEF` | `Z6_3\Nikon - Z6_3 - HE_star.NEF.tiff` |
| `Z50_2\DSC_0012.NEF` | `Z50_2\DSC_0012.NEF.tiff` |

The older Z50 II DX HE / HE* files in this local sample set appear corrupt on
disk; Adobe Camera Raw also fails to open them. The later `DSC_0012.NEF` sample
is the useful DX reference and now validates the DX path.

## What the compression buys

The full-frame raw plane is:

```text
91,411,200 bytes
```

The Z6 III raw plane is:

```text
6064 * 4040 * 2 = 48,997,120 bytes
```

The Z50 II DX raw plane is:

```text
5600 * 3728 * 2 = 41,753,600 bytes
```

The observed sample sizes are:

| File | NEF size | Approx. bpp | Ratio against raw plane |
|---|---:|---:|---:|
| `he.NEF` | 20,721,664 bytes | 3.63 bpp | 4.41:1 |
| `he_star.NEF` | 33,257,984 bytes | 5.82 bpp | 2.75:1 |
| `Nikon - Z9-  HE.NEF` | 24,839,168 bytes | 4.35 bpp | 3.68:1 |
| `Nikon - Z9-  HE_star.NEF` | 35,592,704 bytes | 6.23 bpp | 2.57:1 |
| `Nikon - Z6_3 - HE.NEF` | 13,429,248 bytes | 4.39 bpp | 3.65:1 |
| `Nikon - Z6_3 - HE_star.NEF` | 19,554,816 bytes | 6.39 bpp | 2.51:1 |
| `DSC_0012.NEF` | 16,011,776 bytes | 6.14 bpp | 2.61:1 |

That is the attraction. Nikon gets RAW-domain flexibility while cutting write
bandwidth by a factor large enough to matter in a high-speed camera.

Current local timings, on the reference implementation as of 2026-06-21:

| Input | `he_dump` raw-plane dump | `simple_dcraw -T -6` TIFF pipeline |
|---|---:|---:|
| `he.NEF` | 2.58 s | 12.11 s |
| `he_star.NEF` | 2.62 s | 11.89 s |
| `Nikon - Z9-  HE.NEF` | 2.48 s | 11.81 s |
| `Nikon - Z9-  HE_star.NEF` | 2.65 s | 11.35 s |
| `Nikon - Z6_3 - HE.NEF` | 2.80 s | 8.91 s |
| `Nikon - Z6_3 - HE_star.NEF` | 3.02 s | 8.57 s |

This is not a performance ceiling. The current decoder is still shaped like a
reference implementation. It uses a small linear GTLI table lookup, conservative
scratch buffers, scalar bitstream unpacking, scalar dequantization, scalar 5/3
lifting, and no systematic tile-level parallelism.

The format itself is friendly to optimization. That is part of the point of a
JPEG XS-like structure.

## What remains unsolved

There are five obvious next steps.

First, collect more files. The most valuable samples are more DX HE / HE*
files, different crops, different firmware versions, high-ISO images, very
bright images, and scenes that stress fine detail. Every new group should be
validated at the raw-plane level before anyone looks at a rendered TIFF.

Second, derive the GTLI table more formally. The current table plus derivation
rules work for the observed samples. The likely underlying rule is cleaner than
the table, and finding it would reduce overfitting.

Third, harden corrupt-file handling. A damaged NEF should not be able to drive
the decoder into an access violation. Precinct sizes, line-block sizes, and
bitstream offsets need firm bounds checks throughout the path.

Fourth, optimize. Direct-index GTLI lookup, buffer reuse, SIMD unpacking,
SIMD dequantization, SIMD lifting, and tile parallelism are all natural fits.

Fifth, continue comparing against other converters without mistaking any one
converter for truth. Adobe DNG output is a useful reference, not the original
sensor data.

## The useful way to say the discovery

The careful public claim is:

> Nikon Z8, Z9, Z6 III, and Z50 II HE / HE* NEF decoding has been demonstrated
> without Nikon's closed SDK, using a JPEG XS-like precinct, GCLI/GTLI,
> bit-plane, and 5/3 wavelet reconstruction pipeline.

The equally important caveat is:

> This is not yet a complete claim for every Nikon HE / HE* file. The current
> decoder is validated on the available Z8, Z9, Z6 III, and Z50 II samples to
> raw-plane quantization tolerance, and it needs more samples and independent
> review.

What matters is that the path is now visible. The route from NEF container to
JPEG XS-like marker, from precinct header to dynamic `Bp/Br`, from GCLI and
GTLI to bit-plane coefficients, from 5/3 inverse wavelets to the Bayer raw
plane, is something an open implementation can walk.

That is the real discovery. A format that used to require a closed decoder can
now be studied, explained, tested, and improved in public.

## Methodology and acknowledgements

The evidence in this article comes from local raw-plane comparisons in this
LibRaw checkout. The decoder was exercised through LibRaw's normal
`open_file() + unpack()` path, and the output `raw_image` buffers were compared
against raw planes extracted from Adobe DNG references. The TIFFs mentioned
above were produced with `simple_dcraw -T -6`, but the TIFFs are a secondary
sanity check. The raw-plane comparisons are the main measurement.

The currently validated sample set is:

- Nikon Z8 full-frame HE and HE*,
- Nikon Z9 full-frame HE and HE*,
- Nikon Z6 III full-frame HE and HE*,
- Nikon Z50 II DX HE* (`DSC_0012.NEF`).

Older Z50 II DX HE / HE* local files appear corrupt on disk and also fail in
Adobe Camera Raw, so they are kept only as corrupt-file examples. The newer
`DSC_0012.NEF` file is the valid DX sample used above.

This work stands on the shoulders of LibRaw's existing NEF parsing, DNG
handling, and raw pipeline. The new part is the open Nikon HE / HE* path: the
JPEG XS-like marker detection, precinct parser, GCLI/GTLI decoding,
bit-plane reconstruction, 5/3 inverse wavelet transform, and Bayer merge.
