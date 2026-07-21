#!/usr/bin/env python3
"""gen_test_lib.py — Generate a test library with meaningful visual patterns.

Each "page" is a PNG image file.  mupdf's fz_open_document can open PNG
images natively, so the render (pdf.c) can use them just like PDF pages.

Usage:
    python gen_test_lib.py [--pages N] [--seed S] [--output test_lib]

Creates:
    test_lib/
        features.bin   Binary feature database for the C arrange program
        registry.bin   Binary registry mapping op_id → (img_path, page_idx)
        page_0000.png  Source images (one per page)
        page_0001.png  …
        …

The arrange step reads features.bin to match video frames, and the
render step reads registry.bin to find the corresponding source image
for each tile and renders it via mupdf.
"""

import argparse
import math
import os
import struct
import sys

import cv2
import numpy as np


# ---------------------------------------------------------------------------
#  Feature helper (mirrors build_library.compute_feature)
# ---------------------------------------------------------------------------

def compute_feature(img, n, g, color):
    """Compute an N×N feature vector from an image."""
    h, w = img.shape[:2]
    if color:
        resized = cv2.resize(img, (n, n), interpolation=cv2.INTER_AREA)
        channels = cv2.split(resized)
    else:
        if len(img.shape) == 3:
            img = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        resized = cv2.resize(img, (n, n), interpolation=cv2.INTER_AREA)
        channels = [resized]

    max_val = (1 << g) - 1
    result = np.zeros(n * n * len(channels), dtype=np.uint8)
    for ci, ch in enumerate(channels):
        if g == 8:
            quantized = ch.astype(np.uint8)
        else:
            quantized = ((ch.astype(np.float32) / 255.0) * max_val).astype(np.uint8)
        result[ci * n * n:(ci + 1) * n * n] = quantized.ravel()
    return result


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
    return tile[:page_size, :]


def diag_stripes(page_size, freq):
    x = np.arange(page_size)
    y = np.arange(page_size).reshape(-1, 1)
    period = max(page_size / freq, 1.0)
    pattern = ((x + y) % int(period * 2)).astype(np.float32)
    return (pattern < period).astype(np.uint8) * 255


def radial_gradient(page_size):
    cy, cx = page_size / 2.0, page_size / 2.0
    y, x = np.mgrid[0:page_size, 0:page_size].astype(np.float32)
    dist = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    max_dist = math.sqrt(cx ** 2 + cy ** 2)
    norm = np.clip(dist / max_dist, 0, 1)
    return (norm * 255).astype(np.uint8)


def center_blob(page_size, radius_frac):
    cy, cx = page_size / 2.0, page_size / 2.0
    y, x = np.mgrid[0:page_size, 0:page_size].astype(np.float32)
    dist = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    r = page_size * radius_frac
    return (dist <= r).astype(np.uint8) * 255


def random_binary(page_size, rng):
    return (rng.randint(0, 2, (page_size, page_size)) * 255).astype(np.uint8)


def quantized_noise(page_size, rng, levels=4):
    vals = np.linspace(0, 255, levels, dtype=np.uint8)
    idx = rng.randint(0, levels, (page_size, page_size))
    return vals[idx]


# ---------------------------------------------------------------------------
#  Build the pattern list
# ---------------------------------------------------------------------------

def generate_page(page_idx, page_size, rng):
    """Return a page_size×page_size uint8 grayscale image for the given index."""
    n_types = 22  # total number of distinct pattern slots

    kind = page_idx % n_types

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
    elif kind == 5:
        return h_gradient(page_size)
    elif kind == 6:
        return v_gradient(page_size)
    elif kind == 7:
        return checkerboard(page_size, 2)
    elif kind == 8:
        return checkerboard(page_size, 4)
    elif kind == 9:
        return checkerboard(page_size, 8)
    elif kind == 10:
        return checkerboard(page_size, 16)
    elif kind == 11:
        freq = 2 + (page_idx // n_types) % 16
        return h_stripes(page_size, freq)
    elif kind == 12:
        freq = 2 + (page_idx // n_types) % 16
        return v_stripes(page_size, freq)
    elif kind == 13:
        freq = 2 + (page_idx // n_types) % 16
        return diag_stripes(page_size, freq)
    elif kind == 14:
        return radial_gradient(page_size)
    elif kind == 15:
        r = 0.1 + 0.4 * ((page_idx // n_types) % 10) / 10.0
        return center_blob(page_size, r)
    elif kind == 16:
        return random_binary(page_size, rng)
    elif kind == 17:
        return quantized_noise(page_size, rng, 2)
    elif kind == 18:
        return quantized_noise(page_size, rng, 4)
    elif kind == 19:
        return quantized_noise(page_size, rng, 8)
    elif kind == 20:
        return h_stripes(page_size, 1)
    else:
        return v_stripes(page_size, 1)


# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Generate a test library")
    parser.add_argument("--pages", type=int, default=200,
                        help="Number of pages in the test library (default: 200)")
    parser.add_argument("--seed", type=int, default=42,
                        help="Random seed (default: 42)")
    parser.add_argument("--feat", type=int, default=64,
                        help="Feature grid size N (default: 64)")
    parser.add_argument("--bits", type=int, default=1,
                        help="Bits per cell G (default: 1)")
    parser.add_argument("--page-size", type=int, default=128,
                        help="Width/height of each page image in pixels (default: 128)")
    parser.add_argument("--output", default="test_lib",
                        help="Output directory (default: test_lib)")
    args = parser.parse_args()

    n_pages = args.pages
    seed = args.seed
    N = args.feat
    G = args.bits
    channels = 1
    feat_len = N * N * channels
    page_img_size = args.page_size
    out_dir = args.output

    os.makedirs(out_dir, exist_ok=True)

    rng = np.random.RandomState(seed)

    print(f"Generating {n_pages} test pages (N={N}, G={G}, seed={seed})...")

    features = np.zeros((n_pages, feat_len), dtype=np.uint8)
    img_paths = []

    for i in range(n_pages):
        img = generate_page(i, page_img_size, rng)

        img_path = os.path.join(out_dir, f"page_{i:04d}.png")
        cv2.imwrite(img_path, img)
        img_paths.append(img_path)

        features[i] = compute_feature(img, N, G, color=False)

    feat_path = os.path.join(out_dir, "features.bin")
    with open(feat_path, "wb") as f:
        f.write(struct.pack("<IIII", n_pages, N, G, channels))
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
    print(f"  python arrange.py --video <video> --features {feat_path} --registry {reg_path}")
    print(f"  MISE_FEATURES={feat_path} MISE_REGISTRY={reg_path} mise run arrange -- <video>")

    return 0


if __name__ == "__main__":
    sys.exit(main())
