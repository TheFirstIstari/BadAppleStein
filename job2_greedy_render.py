import os, cv2, numpy as np, pickle, pypdfium2 as pdfium, subprocess
from tqdm import tqdm
from multiprocessing import Pool, cpu_count
import functools

# --- CONFIG ---
MANIFEST_DIR = "manifests_greedy"
LIB_CACHE = "library.pkl"
ATLAS_DIR = "atlas_cache_ultra"
ORIGINAL_VIDEO = "badapple.mp4" 
OUTPUT_MOV = "Bad_Apple_8K_YOUTUBE.mov"

SCALE_FACTOR = 15 
W, H = 512 * SCALE_FACTOR, 384 * SCALE_FACTOR
FPS = 60 
PDF_RENDER_SCALE = 3.0 # Optimal for 8K-16K

# Shared Atlas Global
_ATLAS = {}

def worker_init(atlas_shared):
    """Initializes each worker with access to the shared atlas."""
    global _ATLAS
    _ATLAS = atlas_shared

def render_single_frame(m_file):
    """The core rendering function - optimized for speed."""
    # Create a raw 1-channel canvas
    canvas = np.full((H, W), 255, dtype=np.uint8)
    
    with open(os.path.join(MANIFEST_DIR, m_file), "rb") as f:
        instructions = pickle.load(f)
    
    for x, y, w, h, pid in instructions:
        nx, ny, nw, nh = x*SCALE_FACTOR, y*SCALE_FACTOR, w*SCALE_FACTOR, h*SCALE_FACTOR
        
        # Solid colors are extremely fast
        if pid == -1: canvas[ny:ny+nh, nx:nx+nw] = 0
        elif pid == -2: canvas[ny:ny+nh, nx:nx+nw] = 255
        else:
            # Resize logic: Fast INTER_AREA for text
            source_img = _ATLAS[pid]
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
    pid, path, idx = task
    cp = f"{ATLAS_DIR}/{pid}.png"
    if not os.path.exists(cp):
        try:
            pdf = pdfium.PdfDocument(path)
            page = pdf[idx]
            bitmap = page.render(scale=PDF_RENDER_SCALE).to_numpy()
            gray = cv2.cvtColor(bitmap, cv2.COLOR_BGRA2GRAY)
            # Simple contrast punch
            gray = cv2.normalize(gray, None, 0, 255, cv2.NORM_MINMAX)
            cv2.imwrite(cp, gray)
            pdf.close()
        except: pass

def main():
    if not os.path.exists(ATLAS_DIR): os.makedirs(ATLAS_DIR)
    reg, _ = pickle.load(open(LIB_CACHE, "rb"))
    manifests = sorted([f for f in os.listdir(MANIFEST_DIR) if f.endswith(".bin")])
    
    # 1. PRE-RENDER (Disk I/O Bound)
    needed = {e[4] for f in manifests for e in pickle.load(open(f"{MANIFEST_DIR}/{f}","rb")) if e[4] >= 0}
    tasks = [(i, reg[i][0], reg[i][1]) for i in needed]
    
    print(f"--- Stage 1: Disk Caching ---")
    with Pool(cpu_count()) as p:
        list(tqdm(p.imap_unordered(render_page_worker, tasks), total=len(tasks)))

    # 2. LOAD ATLAS TO RAM (Once)
    print(f"--- Stage 2: Loading Atlas to RAM ---")
    atlas = {}
    for pid in tqdm(needed, desc="Loading"):
        atlas[pid] = cv2.imread(f"{ATLAS_DIR}/{pid}.png", cv2.IMREAD_GRAYSCALE)

    # 3. PARALLEL ASSEMBLY (CPU Bound)
    print(f"--- Stage 3: Assembling 16K Master ---")
    
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
    
    with Pool(num_workers, initializer=worker_init, initargs=(atlas,)) as p:
        # imap returns results in order, allowing smooth piping to FFmpeg
        for frame_bytes in tqdm(p.imap(render_single_frame, manifests), total=len(manifests)):
            proc.stdin.write(frame_bytes)
            
    proc.stdin.close(); proc.wait()
    
    # 4. Final Mux
    if os.path.exists(ORIGINAL_VIDEO):
        subprocess.run(['ffmpeg', '-y', '-i', 'temp_master.mov', '-i', ORIGINAL_VIDEO,
                        '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'copy', '-c:a', 'pcm_s16le',
                        '-shortest', 'BAD_APPLE_8K_YOUTUBE.mov'])

if __name__ == "__main__": main()