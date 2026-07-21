#!/usr/bin/env python3
"""Read and print the first few entries from registry.bin."""
import struct, os

with open("registry.bin", "rb") as f:
    n = struct.unpack("<I", f.read(4))[0]
    print(f"Registry entries: {n}")
    for i in range(min(10, n)):
        page_idx = struct.unpack("<i", f.read(4))[0]
        plen = struct.unpack("<I", f.read(4))[0]
        path = f.read(plen).decode("utf-8")
        print(f"  [{i}] page={page_idx} path='{path}' exists={os.path.exists(path)}")
    print()

# Also check library.pkl if it exists
if os.path.exists("library.pkl"):
    import pickle
    registry, _ = pickle.load(open("library.pkl", "rb"))
    missing = sum(1 for p, i in registry if not os.path.exists(p))
    total = len(registry)
    print(f"library.pkl: {total} entries")
    print(f"Missing PDF files: {missing}/{total}")
    if missing > 0:
        dirs = set(os.path.dirname(p) if os.path.dirname(p) else '.' for p, i in registry)
        print(f"Referenced directories: {dirs}")
