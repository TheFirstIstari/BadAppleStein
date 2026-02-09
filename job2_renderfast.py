import os
import cv2
import numpy as np
import pickle
import pypdfium2 as pdfium
from tqdm import tqdm
from multiprocessing import Pool, cpu_count
import subprocess
from functools import lru_cache

# --- CONFIG ---
MANIFEST_DIR = "manifests"
LIB_CACHE = "library.pkl"

# Revert to your original cache folder so it finds your existing files
ATLAS_DIR = "atlas_cache" 

ORIGINAL_VIDEO = "badapple.mp4" 
OUTPUT_VIDEO = "bad_apple_8K_GRAY_MASTER.mp4"

# 8x SCALE: 4096x3072
SCALE_FACTOR = 8 
W, H = 512 * SCALE_FACTOR, 384 * SCALE_FACTOR
FPS = 30

# Set to None for the full video, or 300 for a 10-second test
TEST_MODE_LIMIT = None 

# High DPI for the PDFs
PDF_RENDER_SCALE = 4.0 

_ATLAS = {}

def worker_init(atlas_data):
    global _ATLAS
    _ATLAS = atlas_data

@lru_cache(maxsize=10000)
def get_tile(pdf_id, nw, nh):
    nw, nh = max(1, nw), max(1, nh)
    return cv2.resize(_ATLAS[pdf_id], (nw, nh), interpolation=cv2.INTER_LANCZOS4)

def render_single_frame(m_file):
    canvas = np.full((H, W), 255, dtype=np.uint8) 
    try:
        with open(os.path.join(MANIFEST_DIR, m_file), "rb") as f:
            instructions = pickle.load(f)
        for x, y, w, h, pdf_id in instructions:
            nx, ny = x * SCALE_FACTOR, y * SCALE_FACTOR
            nw, nh = w * SCALE_FACTOR, h * SCALE_FACTOR
            if ny + nh > H: nh = H - ny
            if nx + nw > W: nw = W - nx

            if pdf_id == -1: canvas[ny:ny+nh, nx:nx+nw] = 0
            elif pdf_id == -2: canvas[ny:ny+nh, nx:nx+nw] = 255
            else:
                canvas[ny:ny+nh, nx:nx+nw] = get_tile(pdf_id, nw, nh)
    except: pass
    
    header = f"P5\n{W} {H}\n255\n".encode()
    return header + canvas.tobytes()

def render_page_worker(task):
    pdf_id, path, pg_idx = task
    cache_path = os.path.join(ATLAS_DIR, f"{pdf_id}.png")
    
    # If it exists, LOAD IT (Forced grayscale)
    if os.path.exists(cache_path):
        img = cv2.imread(cache_path, cv2.IMREAD_GRAYSCALE)
        if img is not None:
            return pdf_id, img
    
    # If not, RENDER IT
    try:
        pdf = pdfium.PdfDocument(path)
        page = pdf[pg_idx]
        bitmap = page.render(scale=PDF_RENDER_SCALE).to_numpy()
        gray = cv2.cvtColor(bitmap, cv2.COLOR_BGRA2GRAY)
        pdf.close()
        cv2.imwrite(cache_path, gray)
        return pdf_id, gray
    except:
        return pdf_id, np.zeros((100, 100), dtype=np.uint8)

def main():
    if not os.path.exists(ATLAS_DIR): 
        os.makedirs(ATLAS_DIR)
        print(f"Created new cache folder: {ATLAS_DIR}")
    
    registry, _ = pickle.load(open(LIB_CACHE, "rb"))
    manifest_files = sorted([f for f in os.listdir(MANIFEST_DIR) if f.endswith(".bin")])
    
    if TEST_MODE_LIMIT:
        print(f"--- TEST MODE ENABLED: {TEST_MODE_LIMIT} frames ---")
        manifest_files = manifest_files[:TEST_MODE_LIMIT]

    # Quick scan of manifests
    unique_ids = set()
    for f in tqdm(manifest_files, desc="Scanning manifests for IDs"):
        try:
            with open(os.path.join(MANIFEST_DIR, f), "rb") as bf:
                for entry in pickle.load(bf):
                    if entry[4] >= 0: unique_ids.add(entry[4])
        except: continue

    # Check how many are already in the folder
    cached_count = sum(1 for i in unique_ids if os.path.exists(os.path.join(ATLAS_DIR, f"{i}.png")))
    print(f"--- Cache Check: {cached_count} / {len(unique_ids)} files found on disk ---")

    atlas = {}
    tasks = [(i, registry[i][0], registry[i][1]) for i in unique_ids]
    
    print(f"--- Loading Atlas (Disk + CPU) ---")
    with Pool(cpu_count()) as p:
        for pdf_id, img in tqdm(p.imap_unordered(render_page_worker, tasks), total=len(tasks)):
            atlas[pdf_id] = img

    print(f"--- Assembling Master 8K Video ---")
    cmd = [
        'ffmpeg', '-y', '-framerate', str(FPS), '-f', 'image2pipe', '-vcodec', 'pgm', '-i', '-',
        '-c:v', 'libx264', '-crf', '0', '-g', '1', '-pix_fmt', 'gray', '-tune', 'stillimage',
        '-fps_mode', 'cfr', OUTPUT_VIDEO
    ]
    
    process = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    with Pool(cpu_count(), initializer=worker_init, initargs=(atlas,)) as p:
        for frame_bytes in tqdm(p.imap(render_single_frame, manifest_files), total=len(manifest_files)):
            process.stdin.write(frame_bytes)
    process.stdin.close()
    process.wait()

    if os.path.exists(ORIGINAL_VIDEO):
        print("--- Final Audio Merge ---")
        subprocess.run(['ffmpeg', '-y', '-i', OUTPUT_VIDEO, '-i', ORIGINAL_VIDEO, 
                        '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'copy', '-c:a', 'aac', '-b:a', '256k', '-shortest', "Bad_Apple_8K_FINAL.mp4"])
        print("Masterpiece complete!")

if __name__ == "__main__":
    main()