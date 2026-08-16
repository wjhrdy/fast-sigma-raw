# Fast Sigma Raw

A small drag-and-drop desktop converter from SIGMA/Foveon X3F files to 16-bit,
three-channel LinearRaw DNG. It runs on macOS and Linux and also
includes a command-line interface. The current development target is the
Merrill/F20 pipeline in SIGMA Photo Pro 6.9.

This is an independent project and is not affiliated with or endorsed by
SIGMA Corporation.

## Download and use

Download the archive for your computer from the
[GitHub Releases page](https://github.com/wjhrdy/fast-sigma-raw/releases) and
extract the entire archive.

1. Open **Fast Sigma Raw** (`Fast Sigma Raw.app` on macOS or
   `fast-sigma-raw-gui` on Linux).
2. Choose a local automatic-import destination and enable removable-media
   watching.
3. Connect a card or drive. Its X3F files are converted into the chosen folder;
   the originals are left untouched.

The watcher first checks for the Merrill camera-card layout
`DCIM/NNNSIGMA/*.X3F`. It does not recursively scan unrelated attached disks.
The app keeps a content-based SHA-256 import history, so reinserting a card or
encountering another copy of the same X3F does not create another DNG. Existing
same-name DNGs in the destination are conservatively treated as prior imports.
The window can be minimized while it watches. Manual drag-and-drop conversion
remains available under **Manual conversion**.

When automatic ejection is enabled, a card is ejected only after every X3F was
successfully imported or recognized as a duplicate. A failed conversion leaves
the card mounted, and the desktop notification reports that it needs attention.

Existing DNG files are not replaced unless **Replace existing DNG files** is
enabled. Conversion runs in the background and processes queued files one at a
time to avoid excessive memory use.

### macOS first launch

Releases currently have an ad-hoc signature but are not Apple-notarized. After
moving **Fast Sigma Raw.app** to Applications, right-click it and choose
**Open**, then confirm **Open**. If Gatekeeper still refuses to launch it, and
only after verifying that the archive came from this repository's Releases
page and matches `SHA256SUMS.txt`, remove the downloaded-file quarantine flag:

```sh
xattr -dr com.apple.quarantine "/Applications/Fast Sigma Raw.app"
```

Separate Apple Silicon (`arm64`) and Intel (`x86_64`) archives are published.
The application requires macOS 12 or newer.

### Linux first launch

Extract the archive and run:

```sh
./fast-sigma-raw-gui
```

If the executable bit was lost during transfer, restore it with
`chmod +x fast-sigma-raw-gui fast-sigma-raw`. The binary targets a conventional
64-bit glibc desktop with OpenGL and either X11 or Wayland.

## Output

The default `merrill` pipeline produces scene-linear D50 ROMM pixels and embeds
a neutral DNG `ProfileToneCurve` plus a measured +1.5 EV baseline exposure. It
does not embed Lightroom Develop adjustments, keeping the linear data available
to a DNG host. It currently reproduces these Merrill stages:

- X3F lossless decoding and black-reference calibration;
- temperature/exposure-gated F20 bad-pixel and bad-cluster repair, including
  the seven native sensor-plane reconstruction modes;
- white-balance gain normalization;
- SPP's exact white-balance-specific radial color-shading correction;
- SPP's two-stage F20 saturation-map restoration and `Sigma_HN` highlight
  neutralization;
- SPP's pre-matrix Normal Despeckle detector: signal-dependent, per-plane
  eight-neighbor peak/valley detection followed by native mode-3 repair;
- Merrill's finite 21x21 CAMF-noise-model bilateral, exact cross-median and
  smoothed edge guide, and normalized local-mean filters;
- F20 dynamic red-defect detection and 31/73/151-scale row-offset suppression;
- the F20 camera-to-ROMM matrix and ColorDQ chroma-residual curves in Sigma's
  native working-space order, preserving shadow gamut for Lightroom;
- lens/spatial-gain correction and the active image crop.

This is an experimental Merrill developer, not yet a bit-exact Photo Pro
replacement. SPP's saturated-detail synthesis and user-adjustable tone
controls are not reproduced yet. The default profile uses an identity tone
curve for a low-contrast Lightroom starting point rather than baking an
appearance curve into the linear samples.

## Processing boundary

The converter deliberately leaves chromatic-aberration correction, sharpening,
detail enhancement, and generic output-space noise reduction to Lightroom.
Those operations are better performed non-destructively in the DNG host.

Pre-matrix Foveon corrections remain part of the converter. They use the
camera's CAMF noise model and the correlated `B-T` and `B-2M+T` sensor-plane
errors, plus Merrill-specific residual and row-pattern behavior. Once the
signed camera matrix has mixed those planes into linear RGB, Lightroom cannot
recover the original plane relationships, so these stages are sensor
reconstruction rather than general-purpose noise reduction.

The Rust executable owns validation, safe output handling, and the CLI. The
decoder and calibration core is the audited, BSD-licensed X3F Tools
implementation, compiled into the binary through a narrow C ABI. No SIGMA
binary or proprietary code is copied or linked.

## Command line

The release archive also contains the `fast-sigma-raw` command-line tool:

```sh
fast-sigma-raw input.X3F output.dng
```

The pipeline can be selected explicitly:

```sh
fast-sigma-raw --pipeline merrill input.X3F output.dng
fast-sigma-raw --pipeline raw input.X3F sensor.dng
fast-sigma-raw --pipeline bmt input.X3F calibration.dng
```

| Pipeline | Pixel data | Intended use |
| --- | --- | --- |
| `merrill` (default) | Developed scene-linear D50 ROMM, cropped | Closest current SPP-like result while retaining linear DNG data and shadow gamut |
| `raw` | Preprocessed Foveon sensor planes | Maximum editability; camera profile and spatial-gain opcode remain in the DNG |
| `bmt` | Normalized, gain-corrected B/M/T planes | Calibration and reverse-engineering diagnostics |

Run `fast-sigma-raw --help` for compression, overwrite, bad-pixel, and
spatial-gain controls. ZIP/Deflate compression is enabled by default.

## Build from source

Rust 1.95 or newer, a C compiler, `pkg-config`, and LibTIFF 4 are required.

On macOS:

```sh
brew install rust pkg-config libtiff
LIBTIFF_STATIC=1 cargo build --release --bins
```

On Ubuntu/Debian:

```sh
sudo apt-get install build-essential pkg-config libtiff-dev \
  libwebp-dev libzstd-dev liblzma-dev libjbig-dev libjpeg-dev \
  libdeflate-dev zlib1g-dev \
  libxkbcommon-dev libwayland-dev libx11-dev libxcursor-dev \
  libxrandr-dev libxi-dev libgl1-mesa-dev
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
LIBTIFF_STATIC=1 cargo build --release --bins
```

The GUI binary is `fast-sigma-raw-gui`; the CLI binary is `fast-sigma-raw`.

## Continuous integration and releases

Every push and pull request builds, lints, and tests the project on macOS and
Ubuntu. Pushing a version tag such as `v0.1.0` runs the release
workflow, which creates:

- macOS Apple Silicon and Intel application archives;
- a Linux x86-64 archive;
- `SHA256SUMS.txt` covering all downloadable archives.

LibTIFF is statically linked on macOS and Linux. macOS builds have an ad-hoc
signature but are not notarized until maintainers configure an Apple Developer
signing identity in the repository.

## Format behavior

- Classic/TRUE/Merrill X3F uses lossless Huffman decoding to interleaved 16-bit
  B/M/T samples.
- Merrill output uses `PhotometricInterpretation=LinearRaw`, three samples per
  pixel, 16 bits, a neutral working-space profile, and an identity profile tone
  curve. No Lightroom/Camera Raw Develop XMP is embedded.
- Capture metadata is translated from X3F PROP/CAMF records into a linked EXIF
  directory, including lens model/make, focal length and 35 mm equivalent,
  aperture, shutter speed, ISO, capture time, body serial, subject distance,
  exposure mode/compensation, metering, flash, white balance, and image ID.
- Quattro decoding is retained from X3F Tools, but the SPP-matching work in this
  repository currently targets Merrill only.

The vendored X3F Tools files retain their original license in
`vendor/x3f-tools/LICENSE`. Reverse-engineering evidence and current gaps are
documented in `docs/reverse-engineering.md`.
