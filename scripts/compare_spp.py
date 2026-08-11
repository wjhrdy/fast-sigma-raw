#!/usr/bin/env python3
"""Compare a developed linear DNG with a SIGMA Photo Pro TIFF export."""

from __future__ import annotations

import argparse
import math
from pathlib import Path

import numpy as np
import tifffile


def largest_page(tiff: tifffile.TiffFile) -> tifffile.TiffPage:
    pages: list[tifffile.TiffPage] = []

    def visit(page: tifffile.TiffPage) -> None:
        pages.append(page)
        if page.pages is not None:
            for child in page.pages:
                visit(child)

    for root in tiff.pages:
        visit(root)
    return max(pages, key=lambda page: math.prod(page.shape[:2]))


def curve_from_pages(tiff: tifffile.TiffFile) -> np.ndarray | None:
    for page in tiff.pages:
        tag = page.tags.get(50940)
        if tag is not None:
            curve = np.asarray(tag.value, dtype=np.float32).reshape(-1, 2)
            if len(curve) < 2 or np.any(np.diff(curve[:, 0]) < 0):
                raise ValueError("invalid DNG ProfileToneCurve")
            return curve
    return None


def read_rgb(
    path: Path, *, apply_orientation: bool = True
) -> tuple[np.ndarray, np.ndarray | None]:
    with tifffile.TiffFile(path) as tiff:
        page = largest_page(tiff)
        curve = curve_from_pages(tiff)
        image = page.asarray()
        if image.ndim != 3 or image.shape[2] < 3:
            raise ValueError(f"{path}: expected an RGB image, got {image.shape}")
        image = image[..., :3]
        maximum = np.iinfo(image.dtype).max if np.issubdtype(image.dtype, np.integer) else 1.0
        rgb = image.astype(np.float32) / maximum
        orientation_tag = page.tags.get(274)
        if orientation_tag is None:
            # DNG writers commonly keep Orientation on IFD0 while the full-size
            # image lives in a SubIFD.
            for root in tiff.pages:
                orientation_tag = root.tags.get(274)
                if orientation_tag is not None:
                    break
        orientation = int(orientation_tag.value) if orientation_tag else 1
        if not apply_orientation:
            return rgb, curve
        if orientation == 3:
            rgb = np.rot90(rgb, 2)
        elif orientation == 6:
            rgb = np.rot90(rgb, 3)
        elif orientation == 8:
            rgb = np.rot90(rgb, 1)
        elif orientation != 1:
            raise ValueError(f"{path}: unsupported TIFF orientation {orientation}")
        return rgb, curve


def srgb_encode(linear: np.ndarray) -> np.ndarray:
    positive = np.maximum(linear, 0.0)
    return np.where(
        positive <= 0.0031308,
        12.92 * positive,
        1.055 * np.power(positive, 1.0 / 2.4) - 0.055,
    )


def romm_d50_to_linear_srgb(image: np.ndarray) -> np.ndarray:
    """Convert linear ROMM/ProPhoto RGB (D50) to linear sRGB (D65)."""
    romm_to_xyz_d50 = np.array(
        [
            [0.7976749, 0.1351917, 0.0313534],
            [0.2880402, 0.7118741, 0.0000857],
            [0.0, 0.0, 0.8252100],
        ],
        dtype=np.float32,
    )
    d50_to_d65 = np.array(
        [
            [0.9555766, -0.0230393, 0.0631636],
            [-0.0282895, 1.0099416, 0.0210077],
            [0.0122982, -0.0204830, 1.3299098],
        ],
        dtype=np.float32,
    )
    xyz_d65_to_srgb = np.array(
        [
            [3.2404542, -1.5371385, -0.4985314],
            [-0.9692660, 1.8760108, 0.0415560],
            [0.0556434, -0.2040259, 1.0572252],
        ],
        dtype=np.float32,
    )
    transform = xyz_d65_to_srgb @ d50_to_d65 @ romm_to_xyz_d50
    return image @ transform.T


def render_candidate(
    image: np.ndarray, curve: np.ndarray | None, mode: str
) -> np.ndarray:
    if mode == "stored":
        return image
    linear = romm_d50_to_linear_srgb(image) if mode == "linear-romm" else image
    if mode == "profile":
        if curve is None:
            raise ValueError("candidate has no DNG ProfileToneCurve")
        linear = np.interp(linear, curve[:, 0], curve[:, 1]).astype(np.float32)
    return srgb_encode(linear)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate", type=Path, help="linear DNG from fast-sigma-raw")
    parser.add_argument("reference", type=Path, help="matching SPP 16-bit TIFF")
    parser.add_argument(
        "--render",
        choices=("linear-romm", "linear-srgb", "profile", "stored"),
        default="linear-romm",
        help="candidate rendering before comparison (default: %(default)s)",
    )
    parser.add_argument(
        "--ignore-orientation",
        action="store_true",
        help="compare stored pixel order (useful when a reference TIFF is physically rotated)",
    )
    args = parser.parse_args()

    candidate, candidate_curve = read_rgb(
        args.candidate, apply_orientation=not args.ignore_orientation
    )
    reference, _ = read_rgb(
        args.reference, apply_orientation=not args.ignore_orientation
    )
    if candidate.shape != reference.shape:
        parser.error(
            f"oriented dimensions differ: {candidate.shape} versus {reference.shape}"
        )

    candidate = render_candidate(candidate, candidate_curve, args.render)
    error = candidate - reference
    absolute = np.abs(error)
    labels = ("R", "G", "B")

    print(f"pixels: {candidate.shape[1]} x {candidate.shape[0]}")
    print(f"render: {args.render}")
    print("channel  candidate_mean  reference_mean  MAE       RMSE      p95_abs")
    for channel, label in enumerate(labels):
        e = error[..., channel]
        a = absolute[..., channel]
        print(
            f"{label:>7}  {candidate[..., channel].mean():14.6f}"
            f"  {reference[..., channel].mean():14.6f}"
            f"  {a.mean():.6f}  {np.sqrt(np.mean(e * e)):.6f}"
            f"  {np.quantile(a, 0.95):.6f}"
        )
    print(
        f"all      {candidate.mean():14.6f}  {reference.mean():14.6f}"
        f"  {absolute.mean():.6f}  {np.sqrt(np.mean(error * error)):.6f}"
        f"  {np.quantile(absolute, 0.95):.6f}"
    )
    zeros = np.mean(candidate <= 0.0, axis=(0, 1))
    clipped = np.mean(candidate >= 1.0, axis=(0, 1))
    print("candidate zero fraction: " + " ".join(f"{x:.6f}" for x in zeros))
    print("candidate clip fraction: " + " ".join(f"{x:.6f}" for x in clipped))

    # Compare hue independently of the reference export's tone settings.  Log
    # channel ratios are invariant to a shared exposure scale and make the
    # direction of a shadow cast explicit: positive R/G is warmer or more
    # magenta; positive B/G is bluer or more magenta.
    ref_y = np.sum(reference * np.array([0.2126, 0.7152, 0.0722]), axis=2)
    epsilon = 1.0 / 65535.0
    cand_rg = np.log2((candidate[..., 0] + epsilon) / (candidate[..., 1] + epsilon))
    cand_bg = np.log2((candidate[..., 2] + epsilon) / (candidate[..., 1] + epsilon))
    ref_rg = np.log2((reference[..., 0] + epsilon) / (reference[..., 1] + epsilon))
    ref_bg = np.log2((reference[..., 2] + epsilon) / (reference[..., 1] + epsilon))
    finite = np.isfinite(cand_rg + cand_bg + ref_rg + ref_bg)
    print("reference-Y band   pixels     R/G bias   B/G bias   chroma MAE (stops)")
    for low, high in ((0.01, 0.05), (0.05, 0.10), (0.10, 0.20),
                      (0.20, 0.40), (0.40, 0.80)):
        selected = finite & (ref_y >= low) & (ref_y < high)
        count = int(np.count_nonzero(selected))
        if count == 0:
            continue
        rg_error = cand_rg[selected] - ref_rg[selected]
        bg_error = cand_bg[selected] - ref_bg[selected]
        chroma_mae = np.mean((np.abs(rg_error) + np.abs(bg_error)) * 0.5)
        print(
            f"{low:4.2f}-{high:<4.2f}  {count:9d}"
            f"  {np.median(rg_error):+9.4f}  {np.median(bg_error):+9.4f}"
            f"  {chroma_mae:18.4f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
