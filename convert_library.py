#!/usr/bin/env python3
"""convert_library.py — one-time converter.

Reads library.pkl (a pickle of (registry, signatures)) and writes the binary
formats consumed by the C arrange program and job2's render loader:

  features.bin   : uint32 n_pages, uint32 N, uint32 G, uint32 channels,
                   then n_pages * (N*N*channels) bytes (uint8 per cell, quantized).
                   For legacy 1-bit signatures: N=64, G=1, channels=1,
                   each bit of the old uint64 words becomes one byte (0 or 1).

  registry.bin   : uint32 n, then for each i:
                     int32  page_idx        (the page_idx int from the tuple)
                     uint32 path_len        (UTF-8 byte length)
                     path_len bytes         (the pdf_path UTF-8)

  signatures.bin : (legacy) uint32 n_pages, then n_pages*64 little-endian
                   uint64 words (4096 bits per signature, row-major).

The C arrange reads features.bin + registry.bin; job2 reads registry.bin to map
op_id (the library page index) -> (pdf_path, page_idx).

library.pkl is produced by an external prep script (not in this repo).
"""
import os
import pickle
import struct
import sys

LIB_CACHE = "library.pkl"
FEAT_OUT = "features.bin"
REG_OUT = "registry.bin"
SIG_OUT = "signatures.bin"


def main():
    if not os.path.exists(LIB_CACHE):
        print(f"[ERROR] Library cache not found: {LIB_CACHE}", file=sys.stderr)
        print("  Run the library preparation script first (not included in this repo)",
              file=sys.stderr)
        return 1

    with open(LIB_CACHE, "rb") as f:
        registry, signatures = pickle.load(f)

    n_pages = len(registry)
    if len(signatures) != n_pages:
        print(f"[ERROR] registry/signatures length mismatch: {n_pages} vs {len(signatures)}",
              file=sys.stderr)
        return 1

    # --- features.bin (new format: N=64, G=1, channels=1) ---
    # Old signatures are (n_pages, 64) uint64. Unpack each bit into a byte.
    N = 64
    G = 1
    channels = 1
    feat_len = N * N * channels  # 4096
    feat_bytes = bytearray()
    for i in range(n_pages):
        row = signatures[i]
        for word in row:
            w = int(word)
            for b in range(64):
                feat_bytes.append(1 if (w >> b) & 1 else 0)
    with open(FEAT_OUT, "wb") as f:
        f.write(struct.pack("<IIII", n_pages, N, G, channels))
        f.write(feat_bytes)
    print(f"[OK] wrote {FEAT_OUT}: {n_pages} pages, N={N} G={G} ch={channels} "
          f"({len(feat_bytes)} bytes)", file=sys.stderr)

    # --- registry.bin ---
    reg_arr = bytearray()
    for i in range(n_pages):
        pdf_path, page_idx = registry[i]
        b = pdf_path.encode("utf-8")
        reg_arr += struct.pack("<ii", int(page_idx), len(b))
        reg_arr += b
    with open(REG_OUT, "wb") as f:
        f.write(struct.pack("<I", n_pages))
        f.write(reg_arr)
    print(f"[OK] wrote {REG_OUT}: {n_pages} entries", file=sys.stderr)

    # --- signatures.bin (legacy format, kept for backward compat) ---
    sig_arr = bytearray()
    for i in range(n_pages):
        row = signatures[i]
        for w in range(64):
            sig_arr += struct.pack("<Q", int(row[w]))
    with open(SIG_OUT, "wb") as f:
        f.write(struct.pack("<I", n_pages))
        f.write(sig_arr)
    print(f"[OK] wrote {SIG_OUT}: {n_pages} pages", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
