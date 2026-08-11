# Reverse-engineering notes

Target: SIGMA Photo Pro 6.9.0.0 for macOS. Addresses below refer to the arm64
slice of that exact bundle and are not stable across releases.

The application executable is a .NET MAUI host. Sensor processing is split
across native libraries in `Contents/MonoBundle/Sensor`:

| Library | Observed generation/path |
| --- | --- |
| `lib98_ClassicDLLForMac.dylib` | pre-TRUE / F13 |
| `lib99_MerillDLLForMac.dylib` | Merrill / F20 |
| `lib95_QuattroDLLForMac.dylib` | Quattro |
| `lib92_TigerDLLForMac.dylib` | later Foveon processing |
| `lib90_TypeB_KN46DLLForMac.dylib` | newer Type-B path |
| `lib90_TypeB_KN410DLLForMac.dylib` | newer Type-B path |

The binaries were inspected headlessly with IDA MCP and Hex-Rays. The Merrill
library contains the following exported decoding primitives:

- `X3FHuffmanImp_F20::GenerateCodes` at `0x87f68`
- `X3FHuffmanImp_F20::BuildDecodeTree` at `0x880c8`
- `X3FHuffmanImp_F20::DecodeNextSymbol` at `0x8894c`

The corresponding F13 functions appear at `0x94af4`, `0x94c54`, and
`0x954d8` in the Classic library. The decompiler shows the same structure in
both generations:

1. canonical codes are generated from per-symbol code lengths;
2. a two-level lookup tree is built for fast decoding;
3. the hot decoder maintains a buffered, MSB-first bit accumulator and consumes
   the exact number of bits associated with the selected leaf;
4. decoded values feed the X3F differential predictors for the three stacked
   sensor planes.

Camera data is block-oriented. For example,
`FCamdataImpl_F13::ParseBlocks` at `0x57a68` validates block sizes, handles
endianness, and dispatches matrix/property payloads. Those payloads contain the
black reference rectangles, active area, saturation/image depth, white-balance
gains, color-correction matrices, and spatial-gain maps needed for a useful
LinearRaw DNG.

## Merrill color pipeline

The following F20 functions anchor the developed color path:

| Function | arm64 address | Result used here |
| --- | ---: | --- |
| `CalculateCAM2RIMMMatrix` | `0x4bf38` | Camera-to-internal working-space construction and neutral normalization |
| `CalculateROMM2FinalMatrix` | `0x4c1f8` | Internal working-space to final output transform |
| `GetTanhLUT` | `0x5d99c` | F20 ColorDQ curve construction |
| `ApplyMatrixAndColorDQ` | `0x73168` | Matrix application followed by chroma-residual ColorDQ |
| `GenerateF20ToneCurve` | `0x57c64` | F20 appearance curve generation |

`CalculateCAM2RIMMMatrix` first multiplies the camera matrix by color-space 11,
SPP's ROMM/RIMM-like internal space. When a valid neutral is supplied, each row
is normalized against a weighted luminance
`L = 0.30 R + 0.55 G + 0.15 B`, with each neutral response floored at `0.25 L`.
It then applies a symmetric channel-mixing matrix whose diagonal is `1 - 2a`
and whose off-diagonal entries are `a`.

`CalculateROMM2FinalMatrix` performs the inverse internal-space conversion and
another symmetric mix. For Merrill Standard, `CMCM_Standard` is the identity
and `CMCC_Standard` is zero. The matrices alone cancel algebraically when the
final space is sRGB, but the entire pipeline does not: ColorDQ and the tone LUT
operate between them, and output clamping occurs only afterward. The developed
DNG therefore retains linear D50 ROMM samples and describes their conversion
with its profile instead of prematurely storing clipped linear sRGB.

ColorDQ is not a 3D lookup table. SPP constructs three symmetric one-dimensional
tanh curves in the 14-bit F20 domain, applies them to per-channel residuals,
forms a `1:2:1` gray residual, and subtracts only the remaining chroma component.
The effect is deliberately small but is now reproduced in the developed path.

## Exact radial color shading

`FIPEngine2_F20::priProcessF20PreprocessStage` at `0xa5a64` reads the
white-balance-specific `WhiteBalanceColorShadingFactor` 2-by-2 CAMF matrix after
applying gains and before denoising. If its coefficients are `[a,b,c,d]`, SPP
computes, for every full-resolution sensor coordinate:

```text
cx = active_left + floor((active_right  - active_left) / 2)
cy = active_top  + floor((active_bottom - active_top)  / 2)
r2 = ((x-cx)^2 + (y-cy)^2) / (half_width^2 + half_height^2)

B' = B * (1 + b*r2 + a*r2^2)
M' = M
T' = T * (1 + d*r2 + c*r2^2)
```

The implementation uses the same CAMF lookup, coordinate system, coefficient
order, plane selection, and pipeline position. On the available DP2 Merrill
SPP comparison, this reduced aggregate linear-sRGB MAE from `0.1494` to `0.1313`
and removed most of the green shadow cast. That TIFF has non-neutral SPP
adjustments (`Shadows +1.9`, `Highlights -1.9`), so its remaining brightness
difference is not treated as a linear-pipeline error.

## Exact Merrill dark reference

The F20 preprocessing path does not average all four nominal optical-black
regions. For each of the three planes it copies `DarkShieldTop`, sorts the
samples, discards the lowest and highest one percent, and averages the central
98 percent. The converter now follows that method for F20 files and retains the
older all-shields estimator only for other sensor generations.

This matters on real DP2 Merrill files because the right-side reference columns
can be illuminated. In `DP2M0838.X3F`, combining all regions produced black
levels near `[39.80, 39.81, 39.91]`; the native top-shield estimator produces
`[31.14, 31.19, 31.14]`. Because the Foveon color matrix has large signed
coefficients, this small raw-domain error caused a large shadow hue error. With
the exact estimator, chroma MAE against the paired SPP TIFF fell from 0.479 to
0.177 stops in its 10–20% reference-luminance band and from 0.156 to 0.084
stops in the 20–40% band.

## Native F20 defect selection and reconstruction

Photo Pro does not treat `BadPixelsF20` as a flat coordinate list. It computes
an exposure/temperature metric from `CaptureShutter`, `SensorTemperature`, and
the four-row CAMF `BadnessTable`, then applies `BPBadnessOffset` and
`BCBadnessOffset` to select the active pixel and cluster classes. The converter
now reproduces that selection, avoiding the former behavior of repairing every
calibration-list variant on every exposure.

Each three-word F20 pixel record also contains an eight-neighbor validity mask
and one of seven reconstruction types. Those types preserve the healthy sensor
plane and reconstruct one or two damaged planes from trimmed local inter-plane
differences, or replace all three planes with their independently trimmed
means. The converter now implements all seven formulas. Dynamic red defects
use the same record path after the quadratic `ColorNoiseModel` detector builds
their surviving-neighbor masks.

`BadClustersF20` is a separate compact stream. Its records encode a class,
rectangle origin and extent, interpolation margin, and packed relative
row/column/channel masks. The stream is now decoded and class-selected. Repair
is iterative and edge-aware, choosing the lowest-gradient horizontal,
vertical, or diagonal support and interpolating only the marked planes. This
matches the native data semantics; Photo Pro's exact global patch-ranking
heuristic remains the one non-bit-exact part of cluster repair.

Across the highlight reference and three additional paired Merrill frames, the
native pixel formulas reduced chroma error slightly in every frame relative to
generic four-neighbor replacement. The aggregate effect is deliberately small:
defect repair changes sparse sensor locations, while later spatial filters
spread only sub-code-value differences into their neighborhoods.

## Native Normal Despeckle

Merrill's normal RGB despeckle is a second sparse sensor-defect stage, not a
generic denoiser. `FIPEngine2_F20::priProcessF20PreprocessStage` invokes
`FPixelProc2_F20::DespeckleRas` at `0x4a62c` after
`F20RestoreHighlights`, before correlation row-offset correction and the main
`DenoiseF20` pipeline. Processing flag `0x4000` skips it.

`DespeckleRas` dispatches `DetectSpecklesRas` at `0x49ac8` over eight row
ranges. Each B/M/T sample is considered independently and must be either
strictly greater than all eight neighbors or strictly less than all eight
neighbors. Its distance beyond the nearest neighbor then has to exceed a
signal-dependent 4096-entry threshold table. The three detected-plane bits are
combined into one F20 bad-pixel record and passed to `ReplaceBadPixelsF20` with
badness class 3, reusing the seven already recovered sensor-plane
reconstruction formulas. Detection completes before any repair, so a repaired
sample cannot affect the classification of a neighboring sample.

The threshold tables use the WB gains, the signed CAMF vector `DespAdjust`
(`10, 10, 5` on the tested DP2 Merrill), and element 1 of the ISO-interpolated
13-column `ISONoiseSettings` row. That ISO multiplier is 1.0 in every DP
Merrill table row, but the converter reads and interpolates it. The three noise
scales are `1.1e-14`, `7.5e-15`, and `3.2e-15`; their floors are `6.4e-7`,
`8.1e-7`, and `4.0e-8`. Classification is performed in Photo Pro's
gain-scaled floating sensor-code domain, with an exact affine conversion from
the converter's biased 14-bit intermediate representation.

The default pass found 4,032, 20,815, 7,656, and 303,388 combined records on
the four ISO-100 regression frames `DP2M0796`, `DP2M0818`, `DP2M0838`, and
`DP2M0879`. The last image contains dense, high-contrast one-pixel branch and
foliage extrema, explaining its much larger native-detector count. A
full-resolution comparison showed isolated color excursions being removed
without visible edge softening; the detector never averages ordinary pixels.

## Merrill denoising

`FPixelProc2_F20::DenoiseF20` at `0x679a4` is a large adaptive residual
pipeline. The decompiled ordering is:

1. dark-shield despeckling and an edge-aware RGB bilateral filter;
2. edge-map construction and optional low-signal neutralization;
3. local mean, resampling, and a monochrome guide;
4. anisotropic diffusion and cross-median/OSF filtering;
5. residual min/max, row/column pattern suppression, and residual mixing;
6. camera matrix plus ColorDQ;
7. residual and sharpening application.

`SetUpNoiseParamsF20` at `0xacdac` interpolates the 13-column CAMF
`ISONoiseSettings` table by estimated ISO, then combines it with
`AdditionalISONoiseSettings` and the denoising slider tables. The developed
converter now performs the same residual-stage decomposition: an edge-aware
local base is formed in `T`, `2(B-T)`, and `B-2M+T`; the original/base intensity
ratio becomes a mono residual map; the map is diffused, stripped of isolated
extrema, and optionally row/column-decorrelated; and the result is converted
to a multiplicative gain. Raw and BMT pipelines do not apply this processing.

The DP2 Merrill CAMF table has now also been decoded directly. At ISO 100 its
primary row is:

```text
100, 1, 1, 10, 0.005, 0.7, 0.555, 4, 1, 0.8, 0, 2, 1
```

and its additional row is:

```text
100, 0.025, 0.025, 0, 0.12, 0.0025, 3, 0, 0, 0, 0, 0, 0
```

The native RGB bilateral does not use `0.005` as a direct code-value threshold.
It derives a range scale of `2047 / (8 / NoiseScaling)`, computes each channel's
variance from the CAMF `ColorNoiseModel` constant, linear, and quadratic terms,
and indexes an `exp(-i/256)` LUT with the summed normalized squared RGB distance.
The default finite-window filter now reproduces that signal-dependent range
metric, interpolates `NoiseScaling` by capture ISO, and uses the native
21-pixel/two-pixel-lattice support at ISO 100. The earlier spatial recursion is
retained only behind a developer differential-testing switch.

`ComputeEdgeMap` at `0x63d90` has also been recovered for the Merrill path.
It first applies a five-sample cross median independently to the three sensor
planes. Centered horizontal and vertical gradients are then divided by the
median center value, the largest squared response across the three planes is
used in each direction, and the combined magnitude is optionally smoothed by
a five-sample cardinal average. After the CAMF locality clamp, the result
indexes the native 2048-entry tanh `EdgeLUT`; those coefficients drive the
normalized forward/backward local-mean FIR.

Two apparent options in the parameter block are misleading in this Photo Pro
build. `DenoiseF20` prints `EdgeMapDoMedianFilter` but the median is
unconditional, and it prints `EdgeMapDoDirectionalFilter` but passes a fixed
zero to a call path that never consumes the flag. `EdgeMapDoSmoothEdge` is a
real branch and defaults on. `EdgeMapAccumulate` and `EdgeMapNoiseModel` are
also honored, although both default off on the tested DP2 Merrill files.

`DarksubDespeckle` has now been reproduced before the compact bilateral stage.
For each interior pixel it transforms the center and its four direct neighbors
to Photo Pro's ROMM-like working space, takes a per-channel median of the five
samples, and transforms the result back. The working-space row normalization
can be omitted without changing the result because median commutes with a
positive per-channel scale. This recovered stage improved deepest-shadow
chroma on six of seven checked paired files while leaving midtones effectively
unchanged.

The residual tail is now independently implemented from the mapped functions.
The converter uses the recovered radius-four and near-unity thresholds to gate
local residual extrema. `RowColumnPatternSuppression` is disabled by the
ISO-100 CAMF row and progressively enabled by the additional ISO table.
`ColorResidualMixing` blends the aggregate original/base ratio with the cleaned
mono ratio using `ResidualMixing`, optionally blends per-plane ratios using
`ResidualColorMixing`, and applies the native reciprocal strength curve. The
gain is retained until after the camera matrix and ColorDQ, matching
`ApplyColorResidual` instead of commuting it through the nonlinear ColorDQ
stage. The still-approximate portion here is the exact native random
permutation used by high-ISO row/column suppression.

On seven paired Merrill frames, the recovered residual tail reduced chroma
error most strongly in the two darkest reference-luminance bands. For example,
DP2M0838 improved from 1.429 to 1.300 stops at 1–5% luminance and from 0.400 to
0.372 stops at 5–10%; its blue-channel zero fraction fell from 0.75% to 0.25%.
Several difficult frames improved by 0.43–0.67 stops in the deepest band.

## Implementation decision

No proprietary implementation was copied, linked, or redistributed. After the
binary survey established the pipeline and fast Huffman design, the converter
uses the independently published, BSD-licensed X3F Tools decoder/calibration
core. That code implements the same documented X3F container and sensor-plane
semantics and is auditable in `vendor/x3f-tools`. A narrow C ABI connects it to
the Rust CLI.

All output modes use three interleaved 16-bit samples per pixel and
`PhotometricInterpretation=LinearRaw`. The `raw` pipeline carries camera
profiles, white balance, black/white levels, ActiveArea, and spatial-gain
opcodes for host-side processing. The developed `merrill` pipeline bakes the
confirmed preprocessing and linear color stages, crops to the active area, and
uses a neutral linear-ROMM DNG profile. Its samples stay linear and its
`ProfileToneCurve` is explicitly the identity curve.

The developed Merrill path now implements both recovered F20 highlight passes.
`F20RestoreHighlights` expands the CAMF `SatMapR/G/B` run maps, clears isolated
flags, reconstructs the clipped top plane from the exposure's unsaturated
top/middle ratio curve, and uses Sigma's 5x5 saturation support and dynamic
4000--7000-code threshold before despeckling. Later, Stage 3 calls `Sigma_HN`:
after undoing the color-correction matrix it measures the top-plane-weighted
signal, then fades matrixed RGB toward the native 25/50/25 neutral value over
2500--3450 codes. This replaces the former final-5% output-space approximation.
On `DP2M0838`, highlight-band chroma error against its SPP TIFF fell from
0.0811 to 0.0244 stops for reference luminance above 90%, while the near-white
green-excess count fell to zero. It still does not synthesize detail after all
three sensor planes saturate.

Paired DNG/SPP TIFF measurements exposed a separate shadow failure mode in the
converter rather than a missing Sigma color adjustment. Sigma matrices into a
D50 ROMM/RIMM float image, applies ColorDQ and tone there, transforms to the
requested output space, and only then clamps. The former converter collapsed
those steps into linear sRGB before unsigned DNG storage. In dark colors the
sRGB matrix could drive red or blue negative while green survived, irreversibly
creating green-only pixels before Lightroom saw the image.

The developed DNG now keeps Sigma's scene-linear D50 ROMM working values and
embeds the corresponding `ColorMatrix1` and native D50 `ForwardMatrix1`.
Lightroom can therefore apply its editable tone and output conversion before
the final gamut boundary. The previous empirical redistribution and measured
`+0.16 EV` red/`-0.25 EV` blue correction are disabled by default and retained
only behind a developer regression switch. On `DP2M0838`, ROMM storage reduced
blue-channel zeroes from 2.21% to 0.42% and green-only stored pixels from 846 to
59 without that correction. The deepest reference-luminance band improved on
all four paired regression frames; near-white green-excess remained zero.

Across 37 matching X3F/SPP TIFF pairs, inverting the embedded Standard profile
curve and comparing moderate, low-saturation pixels produced a median exposure
offset of +1.54 EV (interquartile range +1.24 to +1.73 EV). The developed DNG
therefore stores `BaselineExposure=+1.5`, moving Lightroom's default exposure
zero point without scaling or clipping the scene-linear samples. Raw and BMT
diagnostic modes retain their sensor-derived behavior.

The embedded developed profile declares D65 as `CalibrationIlluminant1` for
its DNG color-matrix reference. Its `ColorMatrix1` maps that reference into the
stored ROMM channels, while `ForwardMatrix1` is Sigma's D50 ROMM-to-XYZ matrix,
as required by the DNG profile connection space.

The recovered F20 Standard curve is deliberately not used as the developed
DNG's default `ProfileToneCurve`. In Lightroom its steep toe and shoulder,
combined with the measured baseline exposure, produced crushed shadows and
excessive highlight contrast. The default `Linear ROMM (Merrill Neutral)`
profile stores an explicit identity curve, leaving tonal contrast editable and
avoiding Adobe's implicit fallback curve.

The developed DNG deliberately contains no XMP packet with Lightroom/Camera
Raw Develop state. Imports therefore begin without preset exposure,
highlight/shadow, HSL, split-toning, or color-grading adjustments. This does not
remove the DNG camera profile, neutral tone curve, baseline exposure, or the
separate EXIF/DNG capture and lens metadata.

The X3F PROP table and selected CAMF values are also mapped into a standards-
compliant EXIF IFD linked from DNG IFD0. This preserves Lightroom-organizing
fields that the original X3F Tools writer omitted: lens make/model, focal
length and 35 mm equivalent, aperture, displayed shutter speed, ISO, camera
serial, capture date/time, subject distance, exposure program and compensation,
metering mode, light source, flash state, white balance, and the original
16-byte image identifier. Known fixed-lens DP Merrill/Quattro IDs are mapped to
their human-readable Sigma names; unknown IDs retain a focal/aperture fallback.

The remaining work for closer SPP parity is the exact tiled CROC solver,
native bad-cluster patch ranking, saturated-detail synthesis, high-ISO residual
permutation details, and a faithful mapping of SPP's adjustable tone controls.
Those stages must be validated against neutral SPP exports; adjusted TIFF
exports are useful appearance references but not ground truth for the linear
pipeline.
