import os, sys, cv2, numpy as np, pickle, pypdfium2 as pdfium, subprocess, time, hashlib
from tqdm import tqdm
from multiprocessing import Pool, cpu_count

def _log(msg):
    """Print to stderr with flush so progress shows even when piped (non-TTY)."""
    print(msg, file=sys.stderr, flush=True)

# --- CONFIG ---
MANIFEST_DIR = "manifests_greedy"
ATLAS_DIR = "atlas_cache"
# Video can be in repo root or local directory
if os.path.exists("../badapple.mp4"):
    ORIGINAL_VIDEO = "../badapple.mp4"
else:
    ORIGINAL_VIDEO = "badapple.mp4"
OUTPUT_MOV = "Bad_Apple_8K_YOUTUBE.mov"

SCALE_FACTOR = 15 
W, H = 512 * SCALE_FACTOR, 384 * SCALE_FACTOR
FPS = 60 
PDF_RENDER_SCALE = 3.0 # Optimal for 8K-16K

# Shared Atlas Global
_ATLAS = {}

# Binary manifest/registry loaders. These replace the old pickle.load sites so
# the render stage can read manifests produced by the C arrange binary.
# Layout (written by arrange.cpp):
#   manifest file: uint32 n, then n * 24-byte records:
#       int32 x, y, w, h, op_id, page_idx
#   registry.bin:  uint32 n, then per i: int32 page_idx, uint32 path_len, path
# op_id is the library page index; registry.bin[i] gives (pdf_path, page_idx).
_REGISTRY = None  # lazily loaded list indexed by op_id

def load_registry():
    """Load registry.bin into a list indexed by op_id -> (pdf_path, page_idx)."""
    global _REGISTRY
    if _REGISTRY is not None:
        return _REGISTRY
    reg_path = os.path.join(MANIFEST_DIR, "registry.bin")
    if not os.path.exists(reg_path):
        reg_path = "registry.bin"
    if not os.path.exists(reg_path):
        _log(f"[ERROR] registry.bin not found (needed to map op_id -> (pdf_path, page_idx))")
        raise FileNotFoundError("registry.bin not found")
    with open(reg_path, "rb") as f:
        data = f.read()
    n = int.from_bytes(data[0:4], "little")
    reg = [None] * n
    off = 4
    for i in range(n):
        page_idx = int.from_bytes(data[off:off+4], "little", signed=True); off += 4
        path_len = int.from_bytes(data[off:off+4], "little"); off += 4
        pdf_path = data[off:off+path_len].decode("utf-8"); off += path_len
        reg[i] = (pdf_path, page_idx)
    _REGISTRY = reg
    return reg

def load_manifest(path):
    """Read a binary manifest into a list of [x, y, w, h, (pdf_path, page_idx)]."""
    reg = load_registry()
    with open(path, "rb") as f:
        data = f.read()
    n = int.from_bytes(data[0:4], "little")
    instrs = []
    off = 4
    rec = 24
    for _ in range(n):
        x = int.from_bytes(data[off:off+4], "little", signed=True)
        y = int.from_bytes(data[off+4:off+8], "little", signed=True)
        w = int.from_bytes(data[off+8:off+12], "little", signed=True)
        h = int.from_bytes(data[off+12:off+16], "little", signed=True)
        op_id = int.from_bytes(data[off+16:off+20], "little", signed=True)
        page_idx = int.from_bytes(data[off+20:off+24], "little", signed=True)
        off += rec
        # op_id is the library page index -> (pdf_path, page_idx) tuple.
        pdf_path, _ = reg[op_id]
        instrs.append([x, y, w, h, (pdf_path, page_idx)])
    return instrs

def cache_key(pdf_path, page_idx):
    """Generate unique cache key from (pdf_path, page_idx) tuple."""
    key_str = f"{pdf_path}_{page_idx}"
    return hashlib.md5(key_str.encode()).hexdigest()[:12]

def worker_init(atlas_shared):
    """Initializes each worker with access to the shared atlas."""
    global _ATLAS
    _ATLAS = atlas_shared

def render_single_frame(m_file):
    """The core rendering function - optimized for speed."""
    # Create a raw 1-channel canvas
    canvas = np.full((H, W), 255, dtype=np.uint8)
    
    with open(os.path.join(MANIFEST_DIR, m_file), "rb") as f:
        instructions = load_manifest(os.path.join(MANIFEST_DIR, m_file))
    
    for x, y, w, h, op in instructions:
        nx, ny, nw, nh = x*SCALE_FACTOR, y*SCALE_FACTOR, w*SCALE_FACTOR, h*SCALE_FACTOR

        # Solid colors are extremely fast
        if op == -1: canvas[ny:ny+nh, nx:nx+nw] = 0
        elif op == -2: canvas[ny:ny+nh, nx:nx+nw] = 255
        else:
            # op is a (pdf_path, page_idx) tuple embedded by job1 — self-contained
            pdf_path, pg_idx = op
            key = (pdf_path, pg_idx)
            source_img = _ATLAS.get(key)
            if source_img is not None:
                # Maintain aspect ratio (letterboxing)
                ih, iw = source_img.shape
                as_src, as_tar = iw/ih, nw/nh
                if as_src > as_tar: tw, th = nw, int(nw/as_src)
                else: th, tw = nh, int(nh*as_src)
                
                # Clamp sizes to 1px min
                tw, th = max(1, tw), max(1, th)
                resized = cv2.resize(source_img, (tw, th), interpolation=cv2.INTER_AREA)
                
                # Plaster centered
                y_off, x_off = (nh-th)//2, (nw-tw)//2
                canvas[ny+y_off:ny+y_off+th, nx+x_off:nx+x_off+tw] = resized
            
    # Piping as raw bytes (No headers = zero CPU overhead for formatting)
    return canvas.tobytes()

def render_page_worker(task):
    """Stage 1: Render PDFs to Disk."""
    # task is a (pdf_path, page_idx) tuple
    pdf_path, page_idx = task
    key = cache_key(pdf_path, page_idx)
    cp = f"{ATLAS_DIR}/{key}.png"
    if not os.path.exists(cp):
        try:
            pdf = pdfium.PdfDocument(pdf_path)
            page = pdf[page_idx]
            bitmap = page.render(scale=PDF_RENDER_SCALE).to_numpy()
            gray = cv2.cvtColor(bitmap, cv2.COLOR_BGRA2GRAY)
            # Simple contrast punch
            gray = cv2.normalize(gray, None, 0, 255, cv2.NORM_MINMAX)
            cv2.imwrite(cp, gray)
            pdf.close()
        except: pass

def main():
    start_time = time.perf_counter()
    
    if not os.path.exists(MANIFEST_DIR):
        _log(f"[ERROR] Manifest directory not found: {MANIFEST_DIR}")
        _log("  Run 'mise run arrange' first to generate manifests")
        return
    
    if not os.path.exists(ATLAS_DIR): os.makedirs(ATLAS_DIR)
    # ponytail: manifests are self-contained (job1 embeds (path,page)),
    # so no LIB_CACHE/registry load needed here.
    manifests = sorted([f for f in os.listdir(MANIFEST_DIR) if f.endswith(".bin")])
    if not manifests:
        _log(f"[ERROR] No manifest files found in {MANIFEST_DIR}")
        return
    needed = set()
    for f in manifests:
        for e in load_manifest(os.path.join(MANIFEST_DIR, f)):
            op = e[4]
            if isinstance(op, tuple):
                needed.add(op)
    tasks = list(needed)  # each is already (pdf_path, page_idx)
    
    _log(f"[PERF] Render prep: {len(manifests)} frames, {len(tasks)} unique PDF pages needed")
    
    # Stage 1: Disk Caching
    stage1_start = time.perf_counter()
    _log(f"--- Stage 1: Disk Caching ---")
    with Pool(cpu_count()) as p:
        list(tqdm(p.imap_unordered(render_page_worker, tasks), total=len(tasks), file=sys.stderr, disable=False, mininterval=1.0, miniters=1))
    stage1_time = time.perf_counter() - stage1_start
    _log(f"[PERF] Stage 1 (PDF render cache) in {stage1_time:.2f}s")
    
    # Stage 2: LOAD ATLAS TO RAM (Once)
    stage2_start = time.perf_counter()
    _log(f"--- Stage 2: Loading Atlas to RAM ---")
    atlas = {}
    for pdf_path, page_idx in tqdm(tasks, desc="Loading", file=sys.stderr, disable=False, mininterval=1.0, miniters=1):
        key = cache_key(pdf_path, page_idx)
        atlas[(pdf_path, page_idx)] = cv2.imread(f"{ATLAS_DIR}/{key}.png", cv2.IMREAD_GRAYSCALE)
    stage2_time = time.perf_counter() - stage2_start
    ram_mb = sum(img.nbytes for img in atlas.values()) / 1024 / 1024
    _log(f"[PERF] Stage 2 (Load to RAM) in {stage2_time:.2f}s | {ram_mb:.1f}MB in memory")
    
    # Stage 3: PARALLEL ASSEMBLY (CPU Bound)
    _log(f"--- Stage 3: Assembling 16K Master ---")
    
    # We use 'rawvideo' format to eliminate PGM/PNG overhead
    cmd = [
        'ffmpeg', '-y', '-framerate', '30', '-f', 'rawvideo', 
        '-pix_fmt', 'gray', '-s', f'{W}x{H}', '-i', '-',
        '-r', '60', '-c:v', 'prores_ks', '-profile:v', '2', 
        '-vendor', 'apl0', '-pix_fmt', 'yuv422p10le', 
        '-movflags', '+faststart', '-fps_mode', 'cfr', 'temp_master.mov'
    ]
    
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, bufsize=10**8)
    
    # Limiting workers to 75% of cores helps prevent RAM spikes in WSL
    num_workers = max(1, int(cpu_count() * 0.75))
    
    stage3_start = time.perf_counter()
    frame_count = 0
    with Pool(num_workers, initializer=worker_init, initargs=(atlas,)) as p:
        # imap returns results in order, allowing smooth piping to FFmpeg
        for frame_bytes in tqdm(p.imap(render_single_frame, manifests), total=len(manifests), file=sys.stderr, disable=False, mininterval=1.0, miniters=1):
            proc.stdin.write(frame_bytes)
            frame_count += 1
            
    proc.stdin.close(); proc.wait()
    stage3_time = time.perf_counter() - stage3_start
    _log(f"[PERF] Stage 3 (Frame assembly) in {stage3_time:.2f}s | {frame_count/stage3_time:.2f} fps output")
    
    # Stage 4: Final Mux
    mux_start = time.perf_counter()
    if os.path.exists(ORIGINAL_VIDEO):
        subprocess.run(['ffmpeg', '-y', '-i', 'temp_master.mov', '-i', ORIGINAL_VIDEO,
                        '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'copy', '-c:a', 'pcm_s16le',
                        '-shortest', 'BAD_APPLE_8K_YOUTUBE.mov'])
    mux_time = time.perf_counter() - mux_start
    if mux_time > 0:
        _log(f"[PERF] Stage 4 (Mux) in {mux_time:.2f}s")
    
    total_time = time.perf_counter() - start_time
    _log(f"[PERF] render complete in {total_time:.2f}s total")

if __name__ == "__main__": main()