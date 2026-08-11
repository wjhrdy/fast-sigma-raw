# Merrill pipeline audit

Target: SIGMA Photo Pro 6.9 arm64, F20 (Merrill) native processing library.

This ledger separates stages confirmed in the binary from stages already
implemented in the independent converter. It exists to prevent an empirical
color correction from being mistaken for a recovered Photo Pro stage.

| Order | Photo Pro F20 stage | Converter status |
| ---: | --- | --- |
| 1 | Decode TRUE/Huffman planes and CAMF/PROP | Implemented |
| 2 | Per-plane `DarkShieldTop` 1–99% trimmed black estimate | Implemented exactly |
| 3 | Dynamic red-defect detection around recorded defects | Implemented: eight-neighbor F20 masks are tested with the quadratic `ColorNoiseModel` in the pre-gain sensor-code domain; surviving-neighbor masks and bottom-plane repair records match the native representation |
| 4 | `gain * (sample - black)` and optional gain tables | Equivalent fixed-point 14-bit representation; negative headroom represented by a common bias |
| 5 | WB-specific quartic radial color shading | Implemented exactly |
| 6 | Recorded bad pixels and bad clusters | F20 badness selection and all seven per-plane bad-pixel formulas implemented from native records; compact cluster records/classes/channel masks decoded and repaired edge-aware, though native cluster patch ranking is not yet bit-exact |
| 7 | F20 saturation-map restoration plus later `Sigma_HN` neutralization | Implemented: compact CAMF run maps, top-plane ratio reconstruction, 5x5 support, exposure threshold, and post-matrix Stage-3 fade |
| 8 | RGB despeckle | Implemented: exact `DespAdjust`/ISO-scaled per-plane noise LUT, strict eight-neighbor peak/valley detector, combined channel masks, native scan margins, and mode-3 F20 reconstruction |
| 9 | Correlation row-offset correction at 31/73/151-pixel scales | Implemented with Sigma's three support scales, edge rejection, 20-observation gate, and magnitude clamp; the native tiled polynomial solver is represented by a robust float correlation estimator |
| 10 | Working-space five-sample cross median (`DarksubDespeckle`) | Implemented; exact Standard-mode neighborhood and transform |
| 11 | Adaptive RGB bilateral | Implemented as the native finite window: ISO-interpolated radius (21x21 at ISO 100), two-pixel `SubSampleKernel` lattice, exact CAMF quadratic range metric, and recovered exponential LUT |
| 12 | Edge-map build, median/directional/smooth options | Implemented for the F20 call path: unconditional five-sample cross median, centered per-plane relative gradients, max-channel accumulation, optional cardinal smoothing, clamping, and exact EdgeLUT. The binary logs the median and directional flags but does not branch on them; its directional argument is fixed to zero |
| 13 | Optional low-signal neutralization | Mapped; normally disabled by the recovered setup path |
| 14 | Normalized local mean and mono guide | Native normalized forward/backward FIR, exact F20 edge-map construction, and recovered EdgeLUT implemented |
| 15 | Anisotropic diffusion and cross median/OSF | Four-neighbor native conduction equation and isolated-extrema suppression implemented |
| 16 | Residual min/max map and row/column suppression | Radius/threshold CAMF model implemented; high-ISO permutation is deterministic rather than native RNG-identical |
| 17 | Multiplicative color/mono residual mixing | Implemented with ISO-interpolated mono/color mix, reciprocal strength curve, and post-matrix application |
| 18 | Camera-to-ROMM matrix followed by ColorDQ | Implemented in Sigma's D50 ROMM working space for Merrill Standard |
| 19 | CA correction | Intentionally delegated to Lightroom; DNG lens metadata/opcodes retained |
| 20 | Sharpness guide and residual application | Sensor residual application implemented after matrix/ColorDQ; sharpening/detail intentionally delegated to Lightroom |
| 21 | Tone, exposure, shadow/highlight, color mode/adjustment | Neutral linear DNG profile with no embedded Lightroom Develop XMP; SPP appearance rendering not baked |
| 22 | Final ROMM/output-space conversion | Intentionally deferred to the DNG host: linear ROMM samples plus the exact D50 ForwardMatrix preserve gamut until Lightroom applies tone/output conversion |

The residual tail (stages 14–17) is enabled by default. Remaining fidelity work
is concentrated in the exact tiled polynomial CROC solver and native
bad-cluster patch ranking rather than in the normal RGB despeckle, finite
bilateral, edge guide, normalized local mean, residual mixing, or highlight
formulas.

## Deliberate host/converter boundary

Lightroom owns chromatic-aberration correction, sharpening/detail enhancement,
and generic luminance/chroma noise reduction. The converter should not bake
those operations into the DNG.

Sensor-specific cleanup remains in scope: defect repair, pre-matrix Foveon
despeckling, CAMF `ColorNoiseModel` filtering, correlated `B-T` and
`B-2M+T` suppression, residual reconstruction, and row/column pattern
suppression. These operate on information that is no longer separable after
the signed Foveon camera matrix, so Lightroom cannot perform an equivalent
correction on the developed linear-RGB DNG.
