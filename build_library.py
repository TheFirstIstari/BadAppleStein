#!/usr/bin/env python3
"""build_library.py — Build a matching library from source PDFs and images.

Usage:
    python build_library.py <sources_dir> --output library.pkl
    python build_library.py <manifest.json> --output library.pkl

The manifest.json is a JSON file with:
    {"sources": [
        {"path": "image.png", "scale": 1.0},
        {"path": "doc.pdf", "page": 0, "scale": 1.0},
        {"path": "image.jpg", "rotations": [0, 90, 180]}  # optional
    ]}

Options:
    --feat N        Feature grid size (default 64)
    --bits G        Bits per cell, 1-8 (default 1 for legacy 1-bit)
    --color         Enable color matching (3 channels, G bits per channel)
    --multi-scale   Emit multiple scales (0.5x, 1.0x, 1.5x, 2.0x)
    --output FILE   Output library.pkl (default: library.pkl)

Outputs:
    library.pkl   Pickle of (registry, features) for backward compatibility.
    features.bin  Binary: uint32 n, uint32 N, uint32 G, uint32 channels,
                  then n * N*N*channels bytes (uint8 per cell, quantized).
    registry.bin  Binary: uint32 n, then per entry int32 page_idx,
                  uint32 path_len, path bytes.
"""
import argparse
import json
import os
import struct
import sys
from pathlib import Path

import cv2
import numpy as np
import pypdfium2 as pdfium


DEFAULT_FEAT = 64
DEFAULT_BITS = 1
MAX_BITS = 8


def compute_feature(img: np.ndarray, n: int, g: int, color: bool) -> np.ndarray:
    """Compute an NxN feature grid from an image.

    Args:
        img: Input image (BGR if color else grayscale, uint8).
        n: Grid size (N x N cells).
        g: Bits per cell (1-8).
        color: If True, process all 3 channels (BGR), else just 1 channel.

    Returns:
        uint8 array of shape (N*N*channels,) with quantized cell values.
        Each cell stores a value 0..2^g-1 packed in one byte.
    """
    h, w = img.shape[:2]
    if color:
        resized = cv2.resize(img, (n, n), interpolation=cv2.INTER_AREA)
        channels = cv2.split(resized)  # B, G, R
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


def render_pdf_page(pdf_path: str, page_idx: int, scale: float = 1.0) -> np.ndarray:
    """Render a single PDF page to a BGR image."""
    pdf = pdfium.PdfDocument(pdf_path)
    page = pdf[page_idx]
    bitmap = page.render(scale=scale).to_numpy()
    return cv2.cvtColor(bitmap, cv2.COLOR_BGRA2BGR)


def render_image(path: str) -> np.ndarray:
    """Read an image file as BGR."""
    return cv2.imread(path, cv2.IMREAD_COLOR)


def main():
    parser = argparse.ArgumentParser(description="Build matching library from PDFs/images")
    parser.add_argument("sources", help="Directory or JSON manifest of sources")
    parser.add_argument("--feat", type=int, default=DEFAULT_FEAT, help=f"Feature grid N (default {DEFAULT_FEAT})")
    parser.add_argument("--bits", type=int, default=DEFAULT_BITS, help=f"Bits per cell G 1-{MAX_BITS} (default {DEFAULT_BITS})")
    parser.add_argument("--color", action="store_true", help="Enable color matching (3 channels)")
    parser.add_argument("--multi-scale", action="store_true", help="Emit 0.5x, 1.0x, 1.5x, 2.0x variants")
    parser.add_argument("--output", default="library.pkl", help="Output library.pkl path")
    args = parser.parse_args()

    n = args.feat
    g = min(args.bits, MAX_BITS)
    color = args.color
    channels = 3 if color else 1
    feat_len = n * n * channels

    sources_list = []

    # Load sources from directory or manifest
    if os.path.isdir(args.sources):
        src_dir = Path(args.sources)
        for ext in ("*.png", "*.jpg", "*.jpeg", "*.bmp", "*.gif", "*.tif", "*.tiff"):
            for img_path in src_dir.glob(str(ext)):
                sources_list.append({"path": str(img_path), "type": "image"})
        for pdf_path in src_dir.glob("*.pdf"):
            pdf = pdfium.PdfDocument(str(pdf_path))
            for page_idx in range(len(pdf)):
                sources_list.append({"path": str(pdf_path), "type": "pdf", "page": page_idx})
            pdf.close()
    elif args.sources.endswith(".json"):
        with open(args.sources) as f:
            data = json.load(f)
        sources_list = data.get("sources", [])
    else:
        print(f"[ERROR] Sources must be a directory or JSON file: {args.sources}", file=sys.stderr)
        return 1

    registry = []
    features = []

    scales = [1.0] if not args.multi_scale else [0.5, 1.0, 1.5, 2.0]

    for src in sources_list:
        path = src["path"]
        try:
            if src.get("type") == "pdf":
                page_idx = src.get("page", 0)
                img = render_pdf_page(path, page_idx, scale=1.0)
            else:
                img = render_image(path)
            if img is None:
                print(f"[WARN] Could not read: {path}", file=sys.stderr)
                continue

            for scale in scales:
                if scale != 1.0:
                    img_scaled = cv2.resize(img, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)
                else:
                    img_scaled = img

                feat = compute_feature(img_scaled, n, g, color)
                registry.append((path, src.get("page", -1) if src.get("type") == "pdf" else -1))
                features.append(feat.tobytes())
                if src.get("rotations"):
                    for rot in src["rotations"]:
                        if rot == 0:
                            continue
                        rot_deg = rot % 360
                        rot_mat = cv2.getRotationMatrix2D((img_scaled.shape[1] / 2, img_scaled.shape[0] / 2), rot_deg, 1.0)
                        img_rot = cv2.warpAffine(img_scaled, rot_mat, (img_scaled.shape[1], img_scaled.shape[0]))
                        feat_rot = compute_feature(img_rot, n, g, color)
                        path_rot = f"{path}_rot{rot}"
                        registry.append((path_rot, src.get("page", -1)))
                        features.append(feat_rot.tobytes())
        except Exception as e:
            print(f"[WARN] Failed processing {path}: {e}", file=sys.stderr)

    if not registry:
        print("[ERROR] No sources processed successfully", file=sys.stderr)
        return 1

    n_pages = len(registry)

    # Write library.pkl for backward compatibility
    with open(args.output, "wb") as f:
        pickle.dump((registry, np.array([np.frombuffer(feat, dtype=np.uint8) for feat in features])), f)

    # Write features.bin: uint32 n, uint32 N, uint32 G, uint32 channels, n*feat_len bytes
    with open("features.bin", "wb") as f:
        f.write(struct.pack("<IIII", n_pages, n, g, channels))
        for feat_bytes in features:
            f.write(feat_bytes)

    # Write registry.bin: uint32 n, then per entry int32 page_idx, uint32 path_len, path
    with open("registry.bin", "wb") as f:
        f.write(struct.pack("<I", n_pages))
        for pdf_path, page_idx in registry:
            b = pdf_path.encode("utf-8")
            f.write(struct.pack("<i", page_idx))
            f.write(struct.pack("<I", len(b)))
            f.write(b)

    print(f"[OK] Built library: {n_pages} pages, N={n}, G={g}, channels={channels}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    import pickle
    sys.exit(main())