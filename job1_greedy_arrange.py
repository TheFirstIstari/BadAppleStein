import os, sys, cv2, numpy as np, ctypes, pickle, time
from tqdm import tqdm

def _log(msg, stream="stderr"):
    """Print with flush so progress shows even when piped (non-TTY).
    Default stderr; the periodic frame heartbeat uses stdout so it does not
    collide with the tqdm progress bar (which owns stderr)."""
    dest = sys.stdout if stream == "stdout" else sys.stderr
    print(msg, file=dest, flush=True)

def _cache_str(timings):
    matched = timings["tiles"]
    hits = timings["cache_hits"]
    total = matched + hits
    if total == 0:
        return "n/a"
    return f"{100*hits/total:.1f}% hit"

# --- CONFIG ---
PDF_ROOT = "Epstein"
MANIFEST_DIR = "manifests_greedy"
LIB_CACHE = "library.pkl"

# SETTINGS FOR OPTIMAL FILL
MAX_BLOCK = 256  # Largest possible PDF page (Backgrounds)

# Optional cap on frames processed (for profiling/benchmarking a subset)
MAX_FRAMES = int(os.environ.get("MAX_FRAMES", "0"))  # 0 = all frames

os.makedirs(MANIFEST_DIR, exist_ok=True)

# Load C library lazily (inside main) since path detection happens there
c_lib = None

def _load_c_lib():
    """Load C matching library with platform-aware path."""
    for path in ["libmatch.dylib", "libmatch.so"]:
        if os.path.exists(path):
            lib = ctypes.CDLL(path)
            lib.match_batch.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64), 
                                       ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
            return lib
    raise FileNotFoundError("C library not found: run 'mise run build-c' first")

def get_bitmask(img):
    resized = cv2.resize(img, (64, 64), interpolation=cv2.INTER_AREA)
    _, binary = cv2.threshold(resized, 127, 255, cv2.THRESH_BINARY)
    return np.packbits(binary).view(np.uint64)

def solve_greedy_accurate(frame, signatures, n_pages, pid_white, pid_black, registry, c_lib, timings=None, tile_cache=None):
    h, w = frame.shape
    _, binary = cv2.threshold(frame, 127, 1, cv2.THRESH_BINARY)
    sum_table = cv2.integral(binary.astype(np.uint8))
    # Coarse grid dimensions (8px cells).
    gh, gw = (h + 7) // 8, (w + 7) // 8
    # Coarse 8px-resolution visited grid (one entry per 8x8 cell). The outer
    # scan only ever lands on 8px-aligned cells, so a full-res visited mask is
    # unnecessary and was allocating big slices on every grow step.
    visited8 = np.zeros((gh, gw), dtype=bool)
    manifest, tiles_to_match, placeholders = [], [], []
    def region_count(cx, cy, cw, ch):
        x0, y0, x1, y1 = cx * 8, cy * 8, (cx + cw) * 8, (cy + ch) * 8
        return (sum_table[y1, x1] - sum_table[y0, x1] - sum_table[y1, x0] + sum_table[y0, x0])

    # A region is "pure" of `color` iff its ON-pixel count equals the full area
    # (for white) or zero (for black). Both are one O(1) integral lookup.
    def is_pure(cx, cy, cw, ch, color):
        cnt = region_count(cx, cy, cw, ch)
        area = cw * ch * 64  # each coarse cell = 8x8 = 64 pixels
        return cnt == area if color == 1 else cnt == 0

    _t_solve = time.perf_counter()
    # Step through the frame in 8px cells
    for cy in range(gh):
        y = cy * 8
        for cx in range(gw):
            x = cx * 8
            if visited8[cy, cx]: continue
            color = binary[y, x]
            mcw, mch = 1, 1  # block size in 8px cells

            # 1. Grow Rectangle greedily up to MAX_BLOCK (in 8px cells)
            max_cells = MAX_BLOCK // 8
            while cx + mcw + 1 <= gw and mcw + 1 <= max_cells:
                # can only extend into yet-unvisited coarse cells
                if not visited8[cy:cy+mch, cx+mcw:cx+mcw+1].any() and is_pure(cx, cy, mcw + 1, mch, color):
                    mcw += 1
                else:
                    break
            while cy + mch + 1 <= gh and mch + 1 <= max_cells:
                if not visited8[cy+mch:cy+mch+1, cx:cx+mcw].any() and is_pure(cx, cy, mcw, mch + 1, color):
                    mch += 1
                else:
                    break

            mw, mh = mcw * 8, mch * 8
            visited8[cy:cy+mch, cx:cx+mcw] = True

            # 2. Assign PDF ID
            if mw >= 32 and mh >= 32:
                # Large block: use the pre-resolved hero PDF (path, page) directly
                manifest.append([x, y, mw, mh, registry[pid_white] if color == 1 else registry[pid_black]])
            else:
                # Edge block: needs visual pattern matching
                bm = get_bitmask(frame[y:y+mh, x:x+mw])
                key = bm.tobytes()  # 512-byte signature, cheap to hash

                # Cross-frame cache: identical tiles reuse the prior match.
                # Bad Apple is highly repetitive, so most tiles are seen before.
                if tile_cache is not None and key in tile_cache:
                    if timings is not None:
                        timings["cache_hits"] += 1
                    manifest.append([x, y, mw, mh, tile_cache[key]])
                else:
                    tiles_to_match.append(bm)
                    placeholders.append((len(manifest), key))
                    manifest.append([x, y, mw, mh, None])
    if timings is not None:
        timings["solve"] += time.perf_counter() - _t_solve

    # 3. Batch Match the (uncached) detail tiles
    if tiles_to_match:
        _t_match = time.perf_counter()
        results = np.zeros(len(tiles_to_match), dtype=np.int32)
        batch = np.array(tiles_to_match, dtype=np.uint64)
        c_lib.match_batch(signatures.ctypes.data_as(ctypes.POINTER(ctypes.c_uint64)), 
                          batch.ctypes.data_as(ctypes.POINTER(ctypes.c_uint64)), 
                          n_pages, len(batch), 
                          results.ctypes.data_as(ctypes.POINTER(ctypes.c_int)))
        for (idx, key), res in zip(placeholders, results):
            page = registry[int(res)]
            manifest[idx][4] = page
            if tile_cache is not None:
                tile_cache[key] = page
        if timings is not None:
            timings["match"] += time.perf_counter() - _t_match
            timings["tiles"] += len(tiles_to_match)
            
    return manifest

def main():
    start_time = time.perf_counter()
    
    # Check library cache exists
    if not os.path.exists(LIB_CACHE):
        _log(f"[ERROR] Library cache not found: {LIB_CACHE}")
        _log("  Run the library preparation script first (not included in this repo)")
        return

    try:
        c_lib = _load_c_lib()
    except FileNotFoundError as e:
        _log(f"[ERROR] {e}")
        return

    registry, signatures = pickle.load(open(LIB_CACHE, "rb"))
    load_time = time.perf_counter()
    _log(f"[PERF] Library loaded in {load_time - start_time:.2f}s ({len(registry)} pages)")
    
    # Resolve video path (can be in repo root or local)
    video_path = "../badapple.mp4" if os.path.exists("../badapple.mp4") else "badapple.mp4"

    # FIND THE "HERO" PDFs (Whitest and Blackest)
    # We do this by summing the bitmasks.
    # High popcount = White/Complex, Low popcount = Black
    # Vectorized popcount over all signatures at once (81k x 512 bytes).
    sig_arr = np.asarray(signatures).reshape(len(registry), -1)
    sig_bytes = np.ascontiguousarray(sig_arr, dtype=np.uint64).view(np.uint8)
    popcounts = np.unpackbits(sig_bytes, axis=1).sum(axis=1)
    pid_white = int(np.argmax(popcounts))
    pid_black = int(np.argmin(popcounts))

    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        _log(f"[ERROR] Cannot open video: {video_path}")
        return
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    if MAX_FRAMES:
        total = min(total, MAX_FRAMES)
        _log(f"[PERF] MAX_FRAMES set: limiting to {total} frames")

    # Phase timing accumulators
    timings = {"read": 0.0, "gray": 0.0, "solve": 0.0, "match": 0.0, "write": 0.0, "tiles": 0, "cache_hits": 0}
    match_calls = 0
    total_tiles = 0
    frames_done = 0
    tile_cache = {}  # cross-frame memo: bitmask bytes -> (pdf_path, page)

    pbar = tqdm(range(total), desc="Dynamic Arranging", file=sys.stderr,
                disable=False, mininterval=1.0, miniters=1, ncols=80, leave=True)
    for i in pbar:
        _t = time.perf_counter()
        ret, frame = cap.read()
        timings["read"] += time.perf_counter() - _t
        if not ret: break

        _t = time.perf_counter()
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        timings["gray"] += time.perf_counter() - _t

        # Solve with Hero PDF assignment for big areas (solve+match timed inside)
        m = solve_greedy_accurate(gray, signatures, len(registry), pid_white, pid_black, registry, c_lib, timings, tile_cache)

        # Count tiles matched (all non-None entries in manifest)
        total_tiles += sum(1 for entry in m if entry[4] is not None)
        match_calls += 1

        _t = time.perf_counter()
        with open(f"{MANIFEST_DIR}/{i:04d}.bin", "wb") as f:
            pickle.dump(m, f)
        timings["write"] += time.perf_counter() - _t
        frames_done += 1

        # Fallback progress heartbeat every 100 frames (helps when TQDM bar
        # is hidden by a non-TTY wrapper such as mise). Goes to stdout so it
        # doesn't collide with the tqdm bar, which owns stderr.
        if frames_done % 100 == 0:
            elapsed = time.perf_counter() - start_time
            _log(f"[PERF] frame {frames_done}/{total} | {frames_done/elapsed:.2f} fps | cache {_cache_str(timings)}", "stdout")
    cap.release()

    total_time = time.perf_counter() - start_time
    fps = frames_done / total_time if total_time else 0
    matched_tiles = timings["tiles"]
    cache_hits = timings["cache_hits"]
    total_detail = matched_tiles + cache_hits
    hit_rate = 100 * cache_hits / total_detail if total_detail else 0
    _log(f"[PERF] arrange complete in {total_time:.2f}s | {fps:.2f} fps | {total_tiles} tiles across {match_calls} frames", "stdout")
    _log(f"[PERF] tile cache: {cache_hits} hits / {total_detail} detail tiles = {hit_rate:.1f}% cache hit rate", "stdout")
    _log("[PERF] phase breakdown (total seconds across all frames):", "stdout")
    for phase in ("read", "gray", "solve", "match", "write"):
        pct = 100 * timings[phase] / total_time if total_time else 0
        _log(f"         {phase:6s}: {timings[phase]:7.2f}s  ({pct:4.1f}%)", "stdout")
    _log(f"         detail tiles sent to C matcher: {matched_tiles}", "stdout")

if __name__ == "__main__":
    main()