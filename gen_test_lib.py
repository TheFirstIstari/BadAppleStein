#!/usr/bin/env python3
"""gen_test_lib.py — Generate a test library with meaningful visual patterns.

Each "page" is a PNG image file.  mupdf's fz_open_document can open PNG
images natively, so the render (pdf.c) can use them just like PDF pages.

The patterns are designed to approximate the visual content of Bad Apple:
high-contrast silhouettes, geometric shapes, gradients, and transitions.

Features use multi-resolution extraction with optional edge detection:
- Grayscale intensity at multiple scales (e.g., 32×32, 64×64, 128×128)
- Sobel edge magnitude at each scale
- Quantized to G bits per cell

Usage:
    python gen_test_lib.py [--pages N] [--seed S] [--output test_lib]

Creates:
    test_lib/
        features.bin   Binary feature database for the C arrange program
        registry.bin   Binary registry mapping op_id → (img_path, page_idx)
        page_0000.png  Source images (one per page)
        page_0001.png  …
        …
"""

import argparse
import math
import os
import struct
import sys

import cv2
import numpy as np


# ---------------------------------------------------------------------------
#  Feature helper (mirrors imgops.c img_compute_feature)
# ---------------------------------------------------------------------------

def compute_feature(img, scales, g, has_edges, color=False):
    """Compute a multi-resolution feature vector from an image.
    
    Args:
        img: Input image (grayscale or BGR)
        scales: List of grid sizes (e.g., [32, 64, 128])
        g: Bits per cell (1-8)
        has_edges: If True, include Sobel edge features
        color: If True, include quantized BGR channels per scale
    
    Returns:
        Feature vector as uint8 numpy array
    """
    if len(img.shape) == 3:
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        bgr = img
    else:
        gray = img
        bgr = None
    
    max_val = (1 << g) - 1
    
    def _quantize(arr):
        if g == 8:
            return arr.astype(np.uint8)
        return np.round(arr.astype(np.float32) / 255.0 * max_val).astype(np.uint8)
    
    features = []
    for N in scales:
        resized_gray = cv2.resize(gray, (N, N), interpolation=cv2.INTER_AREA)
        features.append(_quantize(resized_gray).ravel())
        
        if has_edges:
            gx = cv2.Sobel(resized_gray, cv2.CV_32F, 1, 0, ksize=3)
            gy = cv2.Sobel(resized_gray, cv2.CV_32F, 0, 1, ksize=3)
            magnitude = np.sqrt(gx**2 + gy**2)
            magnitude = np.clip(magnitude, 0, 255).astype(np.uint8)
            features.append(_quantize(magnitude).ravel())
        
        if color and bgr is not None:
            resized_bgr = cv2.resize(bgr, (N, N), interpolation=cv2.INTER_AREA)
            for ch in range(3):
                features.append(_quantize(resized_bgr[:, :, ch]).ravel())
    
    return np.concatenate(features)


# ---------------------------------------------------------------------------
#  Pattern generators  (all return page_size×page_size uint8 grayscale)
# ---------------------------------------------------------------------------

def solid(page_size, value):
    return np.full((page_size, page_size), value, dtype=np.uint8)


def h_gradient(page_size):
    ramp = np.linspace(0, 255, page_size, dtype=np.uint8)
    return np.tile(ramp, (page_size, 1))


def v_gradient(page_size):
    ramp = np.linspace(0, 255, page_size, dtype=np.uint8)
    return np.tile(ramp.reshape(-1, 1), (1, page_size))


def diag_gradient(page_size, invert=False):
    """Diagonal gradient from corner to corner."""
    y, x = np.mgrid[0:page_size, 0:page_size].astype(np.float32)
    total = 2.0 * (page_size - 1)
    norm = (x + y) / total
    if invert:
        norm = 1.0 - norm
    return (np.clip(norm, 0, 1) * 255).astype(np.uint8)


def checkerboard(page_size, cell):
    rows = page_size // cell
    cols = page_size // cell
    pattern = np.zeros((rows, cols), dtype=np.uint8)
    for r in range(rows):
        for c in range(cols):
            pattern[r, c] = 255 if (r + c) % 2 == 0 else 0
    return np.repeat(np.repeat(pattern, cell, axis=0), cell, axis=1)[:page_size, :page_size]


def h_stripes(page_size, freq):
    period = max(page_size // freq, 1)
    band = np.zeros(period, dtype=np.uint8)
    half = period // 2
    band[:half] = 255
    tile = np.tile(band, (page_size, 1))
    return tile[:, :page_size]


def v_stripes(page_size, freq):
    period = max(page_size // freq, 1)
    band = np.zeros(period, dtype=np.uint8)
    half = period // 2
    band[:half] = 255
    tile = np.tile(band.reshape(-1, 1), (1, page_size))
    return tile[:page_size, :page_size]


def diag_stripes(page_size, freq, invert=False):
    x = np.arange(page_size)
    y = np.arange(page_size).reshape(-1, 1)
    period = max(page_size / freq, 1.0)
    pattern = ((x + y) % int(period * 2)).astype(np.float32)
    result = (pattern < period).astype(np.uint8) * 255
    if invert:
        result = 255 - result
    return result


def radial_gradient(page_size, invert=False):
    cy, cx = page_size / 2.0, page_size / 2.0
    y, x = np.mgrid[0:page_size, 0:page_size].astype(np.float32)
    dist = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    max_dist = math.sqrt(cx ** 2 + cy ** 2)
    norm = np.clip(dist / max_dist, 0, 1)
    if invert:
        norm = 1.0 - norm
    return (norm * 255).astype(np.uint8)


def center_blob(page_size, radius_frac):
    cy, cx = page_size / 2.0, page_size / 2.0
    y, x = np.mgrid[0:page_size, 0:page_size].astype(np.float32)
    dist = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    r = page_size * radius_frac
    return (dist <= r).astype(np.uint8) * 255


def offcenter_blob(page_size, cx_frac, cy_frac, radius_frac):
    """Filled circle at an arbitrary position."""
    cy, cx = page_size * cy_frac, page_size * cx_frac
    y, x = np.mgrid[0:page_size, 0:page_size].astype(np.float32)
    dist = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    r = page_size * radius_frac
    return (dist <= r).astype(np.uint8) * 255


def filled_rect(page_size, x0, y0, x1, y1):
    """Filled rectangle (normalized coords 0..1)."""
    img = np.zeros((page_size, page_size), dtype=np.uint8)
    py0 = int(y0 * page_size)
    py1 = int(y1 * page_size)
    px0 = int(x0 * page_size)
    px1 = int(x1 * page_size)
    img[py0:py1, px0:px1] = 255
    return img


def half_fill(page_size, orientation):
    """Half white, half black.  0=top, 1=bottom, 2=left, 3=right."""
    img = np.zeros((page_size, page_size), dtype=np.uint8)
    if orientation == 0:
        img[:page_size // 2, :] = 255
    elif orientation == 1:
        img[page_size // 2:, :] = 255
    elif orientation == 2:
        img[:, :page_size // 2] = 255
    else:
        img[:, page_size // 2:] = 255
    return img


def quarter_fill(page_size, quadrant):
    """One quadrant white, rest black.  0=TL, 1=TR, 2=BL, 3=BR."""
    img = np.zeros((page_size, page_size), dtype=np.uint8)
    half = page_size // 2
    if quadrant == 0:
        img[:half, :half] = 255
    elif quadrant == 1:
        img[:half, half:] = 255
    elif quadrant == 2:
        img[half:, :half] = 255
    else:
        img[half:, half:] = 255
    return img


def diagonal_split(page_size, variant):
    """Diagonal split.  0=TL-dark/BR-light, 1=TL-light/BR-dark,
       2=TR-dark/BL-light, 3=TR-light/BL-dark."""
    y, x = np.mgrid[0:page_size, 0:page_size].astype(np.float32)
    diag = x + y
    mid = page_size
    if variant == 0:
        return ((diag >= mid).astype(np.uint8)) * 255
    elif variant == 1:
        return ((diag < mid).astype(np.uint8)) * 255
    elif variant == 2:
        diag2 = (page_size - 1 - x) + y
        return ((diag2 >= mid).astype(np.uint8)) * 255
    else:
        diag2 = (page_size - 1 - x) + y
        return ((diag2 < mid).astype(np.uint8)) * 255


def concentric_circles(page_size, n_rings):
    """Concentric rings alternating black/white."""
    cy, cx = page_size / 2.0, page_size / 2.0
    y, x = np.mgrid[0:page_size, 0:page_size].astype(np.float32)
    dist = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    max_dist = math.sqrt(cx ** 2 + cy ** 2)
    norm = dist / max_dist
    ring_idx = (norm * n_rings).astype(int) % 2
    return ring_idx.astype(np.uint8) * 255


def border_frame(page_size, thickness):
    """Black with a white border frame (or inverse)."""
    img = np.zeros((page_size, page_size), dtype=np.uint8)
    t = thickness
    img[:t, :] = 255
    img[-t:, :] = 255
    img[:, :t] = 255
    img[:, -t:] = 255
    return img


def inner_frame(page_size, inset, thickness):
    """White with a black inner rectangular frame."""
    img = np.full((page_size, page_size), 255, dtype=np.uint8)
    i0 = inset
    i1 = page_size - inset
    t = thickness
    img[i0:i0 + t, i0:i1] = 0
    img[i1 - t:i1, i0:i1] = 0
    img[i0:i1, i0:i0 + t] = 0
    img[i0:i1, i1 - t:i1] = 0
    return img


def random_binary(page_size, rng):
    return (rng.randint(0, 2, (page_size, page_size)) * 255).astype(np.uint8)


def quantized_noise(page_size, rng, levels=4):
    vals = np.linspace(0, 255, levels, dtype=np.uint8)
    idx = rng.randint(0, levels, (page_size, page_size))
    return vals[idx]


def combined_blob_gradient(page_size, rng):
    """Circle in center + radial gradient overlay."""
    cy, cx = page_size / 2.0, page_size / 2.0
    y, x = np.mgrid[0:page_size, 0:page_size].astype(np.float32)
    dist = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    r = page_size * (0.2 + rng.random() * 0.25)
    blob = (dist <= r).astype(np.float32)
    max_dist = math.sqrt(cx ** 2 + cy ** 2)
    grad = np.clip(dist / max_dist, 0, 1)
    combined = np.clip(blob * 255 + (1 - blob) * grad * 128, 0, 255)
    return combined.astype(np.uint8)


def elliptical_blob(page_size, rx_frac, ry_frac, angle_deg):
    """Rotated ellipse."""
    cy, cx = page_size / 2.0, page_size / 2.0
    y, x = np.mgrid[0:page_size, 0:page_size].astype(np.float32)
    dx = x - cx
    dy = y - cy
    angle = math.radians(angle_deg)
    cos_a, sin_a = math.cos(angle), math.sin(angle)
    rx = dx * cos_a + dy * sin_a
    ry = -dx * sin_a + dy * cos_a
    rx_r = page_size * rx_frac
    ry_r = page_size * ry_frac
    mask = (rx / rx_r) ** 2 + (ry / ry_r) ** 2
    return (mask <= 1.0).astype(np.uint8) * 255


def cross_pattern(page_size, thickness):
    """White cross on black background."""
    img = np.zeros((page_size, page_size), dtype=np.uint8)
    t = thickness
    mid = page_size // 2
    img[mid - t // 2:mid + t // 2, :] = 255
    img[:, mid - t // 2:mid + t // 2] = 255
    return img


def diamond(page_size, size_frac):
    """Centered diamond shape."""
    cy, cx = page_size / 2.0, page_size / 2.0
    y, x = np.mgrid[0:page_size, 0:page_size].astype(np.float32)
    r = page_size * size_frac
    mask = (np.abs(x - cx) + np.abs(y - cy)) <= r
    return mask.astype(np.uint8) * 255


def triangle(page_size, variant):
    """Right triangle in different orientations. 0=BL, 1=BR, 2=TL, 3=TR."""
    y, x = np.mgrid[0:page_size, 0:page_size].astype(np.float32)
    n = page_size - 1
    if variant == 0:  # bottom-left
        mask = (x / n + y / n) <= 1.0
    elif variant == 1:  # bottom-right
        mask = ((n - x) / n + y / n) <= 1.0
    elif variant == 2:  # top-left
        mask = (x / n + (n - y) / n) <= 1.0
    else:  # top-right
        mask = ((n - x) / n + (n - y) / n) <= 1.0
    return mask.astype(np.uint8) * 255


def ring(page_size, inner_frac, outer_frac):
    """Annular ring."""
    cy, cx = page_size / 2.0, page_size / 2.0
    y, x = np.mgrid[0:page_size, 0:page_size].astype(np.float32)
    dist = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    r_inner = page_size * inner_frac
    r_outer = page_size * outer_frac
    mask = (dist >= r_inner) & (dist <= r_outer)
    return mask.astype(np.uint8) * 255


# ---------------------------------------------------------------------------
#  Build the pattern list — 64 distinct types with per-page variation
# ---------------------------------------------------------------------------

def generate_page(page_idx, page_size, rng):
    """Return a page_size×page_size uint8 grayscale image for the given index."""
    n_types = 64

    kind = page_idx % n_types
    cycle = page_idx // n_types  # which repetition (for per-page variation)

    # ── Solids (0-4) ──────────────────────────────────────────────────
    if kind == 0:
        return solid(page_size, 0)
    elif kind == 1:
        return solid(page_size, 64)
    elif kind == 2:
        return solid(page_size, 128)
    elif kind == 3:
        return solid(page_size, 192)
    elif kind == 4:
        return solid(page_size, 255)

    # ── Gradients (5-10) ──────────────────────────────────────────────
    elif kind == 5:
        return h_gradient(page_size)
    elif kind == 6:
        return v_gradient(page_size)
    elif kind == 7:
        return diag_gradient(page_size, invert=False)
    elif kind == 8:
        return diag_gradient(page_size, invert=True)
    elif kind == 9:
        return radial_gradient(page_size, invert=False)
    elif kind == 10:
        return radial_gradient(page_size, invert=True)

    # ── Checkerboards (11-14) ─────────────────────────────────────────
    elif kind == 11:
        return checkerboard(page_size, 4)
    elif kind == 12:
        return checkerboard(page_size, 8)
    elif kind == 13:
        return checkerboard(page_size, 16)
    elif kind == 14:
        return checkerboard(page_size, 32)

    # ── Stripes (15-22) ───────────────────────────────────────────────
    elif kind == 15:
        freq = 2 + cycle % 8
        return h_stripes(page_size, freq)
    elif kind == 16:
        freq = 2 + cycle % 8
        return v_stripes(page_size, freq)
    elif kind == 17:
        freq = 2 + cycle % 8
        return diag_stripes(page_size, freq, invert=False)
    elif kind == 18:
        freq = 2 + cycle % 8
        return diag_stripes(page_size, freq, invert=True)
    elif kind == 19:
        return h_stripes(page_size, 1)
    elif kind == 20:
        return v_stripes(page_size, 1)
    elif kind == 21:
        return diag_stripes(page_size, 1, invert=False)
    elif kind == 22:
        return diag_stripes(page_size, 1, invert=True)

    # ── Center blobs (23-27) ──────────────────────────────────────────
    elif kind == 23:
        return center_blob(page_size, 0.15)
    elif kind == 24:
        return center_blob(page_size, 0.25)
    elif kind == 25:
        return center_blob(page_size, 0.35)
    elif kind == 26:
        return center_blob(page_size, 0.45)
    elif kind == 27:
        return center_blob(page_size, 0.55)

    # ── Off-center blobs (28-35) — 4 corners + 4 edge centers ─────────
    elif kind == 28:
        return offcenter_blob(page_size, 0.25, 0.25, 0.2)
    elif kind == 29:
        return offcenter_blob(page_size, 0.75, 0.25, 0.2)
    elif kind == 30:
        return offcenter_blob(page_size, 0.25, 0.75, 0.2)
    elif kind == 31:
        return offcenter_blob(page_size, 0.75, 0.75, 0.2)
    elif kind == 32:
        return offcenter_blob(page_size, 0.5, 0.25, 0.15)
    elif kind == 33:
        return offcenter_blob(page_size, 0.5, 0.75, 0.15)
    elif kind == 34:
        return offcenter_blob(page_size, 0.25, 0.5, 0.15)
    elif kind == 35:
        return offcenter_blob(page_size, 0.75, 0.5, 0.15)

    # ── Half fills (36-39) ────────────────────────────────────────────
    elif kind == 36:
        return half_fill(page_size, 0)  # top
    elif kind == 37:
        return half_fill(page_size, 1)  # bottom
    elif kind == 38:
        return half_fill(page_size, 2)  # left
    elif kind == 39:
        return half_fill(page_size, 3)  # right

    # ── Quarter fills (40-43) ─────────────────────────────────────────
    elif kind == 40:
        return quarter_fill(page_size, 0)  # TL
    elif kind == 41:
        return quarter_fill(page_size, 1)  # TR
    elif kind == 42:
        return quarter_fill(page_size, 2)  # BL
    elif kind == 43:
        return quarter_fill(page_size, 3)  # BR

    # ── Diagonal splits (44-47) ───────────────────────────────────────
    elif kind == 44:
        return diagonal_split(page_size, 0)
    elif kind == 45:
        return diagonal_split(page_size, 1)
    elif kind == 46:
        return diagonal_split(page_size, 2)
    elif kind == 47:
        return diagonal_split(page_size, 3)

    # ── Concentric circles (48-50) ────────────────────────────────────
    elif kind == 48:
        return concentric_circles(page_size, 2)
    elif kind == 49:
        return concentric_circles(page_size, 3)
    elif kind == 50:
        return concentric_circles(page_size, 5)

    # ── Border frames (51-52) ─────────────────────────────────────────
    elif kind == 51:
        return border_frame(page_size, max(1, page_size // 16))
    elif kind == 52:
        return border_frame(page_size, max(1, page_size // 8))

    # ── Diamonds (53-55) ──────────────────────────────────────────────
    elif kind == 53:
        return diamond(page_size, 0.2)
    elif kind == 54:
        return diamond(page_size, 0.35)
    elif kind == 55:
        return diamond(page_size, 0.48)

    # ── Triangles (56-59) ─────────────────────────────────────────────
    elif kind == 56:
        return triangle(page_size, 0)
    elif kind == 57:
        return triangle(page_size, 1)
    elif kind == 58:
        return triangle(page_size, 2)
    elif kind == 59:
        return triangle(page_size, 3)

    # ── Cross / Rings (60-63) ─────────────────────────────────────────
    elif kind == 60:
        return cross_pattern(page_size, max(1, page_size // 8))
    elif kind == 61:
        return ring(page_size, 0.2, 0.4)
    elif kind == 62:
        return ring(page_size, 0.3, 0.45)
    else:
        return elliptical_blob(page_size, 0.3, 0.15, 45)


# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Generate a test library")
    parser.add_argument("--pages", type=int, default=2000,
                        help="Number of pages in the test library (default: 2000)")
    parser.add_argument("--seed", type=int, default=42,
                        help="Random seed (default: 42)")
    parser.add_argument("--bits", type=int, default=1,
                        help="Bits per cell G (default: 1)")
    parser.add_argument("--no-edges", action="store_true",
                        help="Disable edge detection features")
    parser.add_argument("--color", action="store_true",
                        help="Include 3-channel BGR color features per scale")
    parser.add_argument("--scales", type=str, default="32,64,128",
                        help="Comma-separated scale levels (default: 32,64,128)")
    parser.add_argument("--page-size", type=int, default=1024,
                        help="Width/height of each page image in pixels (default: 1024)")
    parser.add_argument("--output", default="test_lib",
                        help="Output directory (default: test_lib)")
    args = parser.parse_args()

    n_pages = args.pages
    seed = args.seed
    G = args.bits
    has_edges = not args.no_edges
    color = args.color
    scales_list = [int(x) for x in args.scales.split(",")]
    channels_per_scale = (1 + (1 if has_edges else 0) + (3 if color else 0))
    feat_len = sum(N * N * channels_per_scale for N in scales_list)
    page_img_size = args.page_size
    out_dir = args.output

    os.makedirs(out_dir, exist_ok=True)

    rng = np.random.RandomState(seed)

    print(f"Generating {n_pages} test pages (scales={scales_list}, G={G}, edges={has_edges}, color={color}, seed={seed}, size={page_img_size}×{page_img_size})...")

    features = np.zeros((n_pages, feat_len), dtype=np.uint8)
    img_paths = []

    for i in range(n_pages):
        img = generate_page(i, page_img_size, rng)

        img_path = os.path.join(out_dir, f"page_{i:04d}.png")
        cv2.imwrite(img_path, img)
        img_paths.append(img_path)

        features[i] = compute_feature(img, scales_list, G, has_edges, color=color)

    feat_path = os.path.join(out_dir, "features.bin")
    with open(feat_path, "wb") as f:
        # New multi-resolution format header
        f.write(struct.pack("<I", n_pages))           # n_pages
        f.write(struct.pack("<I", feat_len))           # feat_len
        f.write(struct.pack("<I", G))                  # G
        f.write(struct.pack("<I", len(scales_list)))   # n_scales
        for s in scales_list:
            f.write(struct.pack("<I", s))              # scale_i
        f.write(struct.pack("<I", has_edges))          # has_edges
        f.write(struct.pack("<I", 3 if color else 1))  # channels
        f.write(features.tobytes())
    print(f"  wrote {feat_path}  ({n_pages} pages, {feat_len}-byte features)")

    reg_path = os.path.join(out_dir, "registry.bin")
    with open(reg_path, "wb") as f:
        f.write(struct.pack("<I", n_pages))
        for i in range(n_pages):
            rel = img_paths[i]
            b = rel.encode("utf-8")
            f.write(struct.pack("<i", 0))       # page_idx (unused for images)
            f.write(struct.pack("<I", len(b)))  # path length
            f.write(b)                           # path
    print(f"  wrote {reg_path}  ({n_pages} entries → PNG images)")

    print()
    print("To use this library:")
    print(f"  badapplestein encode <video> output.mov --features {feat_path} --registry {reg_path}")
    print(f"  badapplestein build <sources_dir> --library .")

    return 0


if __name__ == "__main__":
    sys.exit(main())
