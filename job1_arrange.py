import os
import cv2
import numpy as np
import pypdfium2 as pdfium
import ctypes
import pickle
from tqdm import tqdm
from concurrent.futures import ProcessPoolExecutor

# --- CONFIG ---
PDF_ROOT = "Epstein"          # Matches your folder name
VIDEO_PATH = "badapple.mp4"   # Make sure you renamed your video to this!
MANIFEST_DIR = "manifests"
LIB_CACHE = "library.pkl"
LIB_PATH = "./libmatch.so"    # Linux shared object for WSL

# --- LOAD C LIBRARY ---
if not os.path.exists(LIB_PATH):
    print(f"ERROR: {LIB_PATH} not found. Did you compile match.c?")
    exit()

c_lib = ctypes.CDLL(LIB_PATH)
c_lib.match_batch.argtypes = [
    ctypes.POINTER(ctypes.c_uint64), # lib
    ctypes.POINTER(ctypes.c_uint64), # targets
    ctypes.c_int,                   # n_pages
    ctypes.c_int,                   # num_targets
    ctypes.POINTER(ctypes.c_int)    # results
]

# --- WORKER FOR PARALLEL PDF PROCESSING ---
def render_worker(pdf_path):
    sigs = []
    meta = []
    try:
        pdf = pdfium.PdfDocument(pdf_path)
        for i in range(len(pdf)):
            page = pdf[i]
            # Fast render scale
            bitmap = page.render(scale=0.3).to_numpy()
            gray = cv2.cvtColor(bitmap, cv2.COLOR_BGRA2GRAY)
            resized = cv2.resize(gray, (64, 64), interpolation=cv2.INTER_AREA)
            _, binary = cv2.threshold(resized, 127, 255, cv2.THRESH_BINARY)
            sigs.append(np.packbits(binary).view(np.uint64))
            meta.append((pdf_path, i))
        pdf.close()
    except Exception:
        pass
    return sigs, meta

def build_index():
    if os.path.exists(LIB_CACHE):
        print(f"--- Loading library from cache: {LIB_CACHE} ---")
        return pickle.load(open(LIB_CACHE, "rb"))

    all_pdfs = []
    for root, _, files in os.walk(PDF_ROOT):
        for file in files:
            if file.lower().endswith(".pdf"):
                all_pdfs.append(os.path.join(root, file))
    
    if not all_pdfs:
        print(f"ERROR: No PDFs found in {PDF_ROOT}")
        return [], np.array([])

    registry, signatures = [], []
    print(f"--- Indexing {len(all_pdfs)} PDFs using Parallel Multiprocessing ---")
    
    with ProcessPoolExecutor() as executor:
        results = list(tqdm(executor.map(render_worker, all_pdfs), total=len(all_pdfs), desc="Ingesting PDFs"))
        
    for sig_list, meta_list in results:
        signatures.extend(sig_list)
        registry.extend(meta_list)

    data = (registry, np.array(signatures, dtype=np.uint64))
    with open(LIB_CACHE, "wb") as f:
        pickle.dump(data, f)
    return data

# --- MAIN ARRANGER ---
def run_arrangement():
    if not os.path.exists(MANIFEST_DIR):
        os.makedirs(MANIFEST_DIR)

    registry, signatures = build_index()
    n_pages = len(registry)
    if n_pages == 0: return

    cap = cv2.VideoCapture(VIDEO_PATH)
    if not cap.isOpened():
        print(f"ERROR: Could not open {VIDEO_PATH}. Did you rename the video file?")
        return

    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    pbar = tqdm(total=total_frames, desc="Analyzing Video")

    frame_idx = 0
    while cap.isOpened():
        ret, frame = cap.read()
        if not ret: break
        
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        tiles_to_match = []
        manifest_template = []
        
        def collect_tiles(x, y, w, h):
            tile = gray[y:y+h, x:x+w]
            var = np.var(tile)
            # Threshold for solid areas
            if var < 5:
                manifest_template.append([x, y, w, h, -1 if np.mean(tile) < 127 else -2])
            # Threshold for leaf nodes (match against PDFs)
            elif w <= 32:
                resized = cv2.resize(tile, (64, 64), interpolation=cv2.INTER_AREA)
                _, binary = cv2.threshold(resized, 127, 255, cv2.THRESH_BINARY)
                tiles_to_match.append(np.packbits(binary).view(np.uint64))
                manifest_template.append([x, y, w, h, None])
            # Recursive split
            else:
                hw, hh = w // 2, h // 2
                collect_tiles(x, y, hw, hh)
                collect_tiles(x + hw, y, hw, hh)
                collect_tiles(x, y + hh, hw, hh)
                collect_tiles(x + hw, y + hh, hw, hh)

        collect_tiles(0, 0, 512, 384)

        if tiles_to_match:
            batch_np = np.array(tiles_to_match, dtype=np.uint64)
            results = np.zeros(len(tiles_to_match), dtype=np.int32)
            
            # CALL THE C ENGINE
            c_lib.match_batch(
                signatures.ctypes.data_as(ctypes.POINTER(ctypes.c_uint64)),
                batch_np.ctypes.data_as(ctypes.POINTER(ctypes.c_uint64)),
                n_pages, len(tiles_to_match),
                results.ctypes.data_as(ctypes.POINTER(ctypes.c_int))
            )
            
            res_idx = 0
            for i in range(len(manifest_template)):
                if manifest_template[i][4] is None:
                    manifest_template[i][4] = int(results[res_idx])
                    res_idx += 1

        with open(f"{MANIFEST_DIR}/{frame_idx:04d}.bin", "wb") as f:
            pickle.dump(manifest_template, f)
        
        frame_idx += 1
        pbar.update(1)

    cap.release()
    pbar.close()

if __name__ == "__main__":
    run_arrangement()