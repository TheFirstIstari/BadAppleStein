#!/usr/bin/env python3
"""gen_test_lib.py — Generate a test library with random features and source images.

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

    # Generate random features + page images
    print(f"Generating {n_pages} test pages (N={N}, G={G}, seed={seed})...")

    features = np.zeros((n_pages, feat_len), dtype=np.uint8)
    img_paths = []

    for i in range(n_pages):
        # Base pattern: random binary grid at feature resolution
        base = rng.randint(0, 2, (N, N)).astype(np.uint8)

        # Add structured patterns so matching isn't purely random:
        # - a vertical/horizontal stripe that varies by page index
        # - a corner patch
        stripe_dir = (i // 10) % 2
        if stripe_dir == 0:
            base[:, i % N] = 1
        else:
            base[i % N, :] = 1
        base[0:8, 0:8] = (i % 2)

        feature_flat = base.ravel()
        features[i] = feature_flat

        # Upscale to page_img_size for the source image
        upscale = page_img_size // N
        img = np.repeat(np.repeat(base * 255, upscale, axis=0), upscale, axis=1).astype(np.uint8)

        # Write PNG
        img_path = os.path.join(out_dir, f"page_{i:04d}.png")
        cv2.imwrite(img_path, img)
        img_paths.append(img_path)

    # Write features.bin
    feat_path = os.path.join(out_dir, "features.bin")
    with open(feat_path, "wb") as f:
        f.write(struct.pack("<IIII", n_pages, N, G, channels))
        f.write(features.tobytes())
    print(f"  wrote {feat_path}  ({n_pages} pages, {feat_len}-byte features)")

    # Write registry.bin (relative paths so it works from CWD = BadAppleStein/)
    reg_path = os.path.join(out_dir, "registry.bin")
    with open(reg_path, "wb") as f:
        f.write(struct.pack("<I", n_pages))
        for i in range(n_pages):
            rel = img_paths[i]  # already relative to CWD
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

