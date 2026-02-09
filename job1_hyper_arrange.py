import os, cv2, numpy as np, pickle, ctypes, hashlib
from tqdm import tqdm
from multiprocessing import Pool, cpu_count, Manager

# --- CONFIG ---
VIDEO_PATH = "badapple.mp4"   
MANIFEST_DIR = "manifests_greedy"
LIB_CACHE = "library.pkl"
LIB_PATH = "./libmatch.so"
os.makedirs(MANIFEST_DIR, exist_ok=True)

# PARAMS
MIN_BLOCK, MAX_BLOCK = 16, 256

# C setup
c_lib = ctypes.CDLL(LIB_PATH)
c_lib.match_batch.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64), 
                               ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]

def get_bitmask(img):
    resized = cv2.resize(img, (64, 64), interpolation=cv2.INTER_AREA)
    _, binary = cv2.threshold(resized, 127, 255, cv2.THRESH_BINARY)
    return np.packbits(binary).view(np.uint64)

# Global variables for workers
_SIGS = None
_NPAGES = 0
_W_POOL = []
_B_POOL = []

def init_worker(sigs, n_pages, w_pool, b_pool):
    global _SIGS, _NPAGES, _W_POOL, _B_POOL
    _SIGS = sigs
    _NPAGES = n_pages
    _W_POOL = w_pool
    _B_POOL = b_pool

def solve_frame_parallel(task):
    frame_idx, frame = task
    h, w = frame.shape
    manifest = []
    edge_tasks = []

    def split_rect(x, y, rw, rh):
        region = frame[y:y+rh, x:x+rw]
        std = np.std(region)
        if (std < 2.0 and rw <= MAX_BLOCK) or rw <= MIN_BLOCK or rh <= MIN_BLOCK:
            if std < 2.0:
                pid = _W_POOL[frame_idx % len(_W_POOL)] if np.mean(region) > 127 else _B_POOL[frame_idx % len(_B_POOL)]
                manifest.append([x, y, rw, rh, pid])
            else:
                manifest.append([x, y, rw, rh, None])
                edge_tasks.append((x, y, rw, rh, len(manifest)-1))
        else:
            hw, hh = rw // 2, rh // 2
            split_rect(x, y, hw, hh)
            split_rect(x + hw, y, hw, hh)
            split_rect(x, y + hh, hw, hh)
            split_rect(x + hw, y + hh, hw, hh)

    for y in range(0, h, 128):
        for x in range(0, w, 128):
            split_rect(x, y, min(128, w-x), min(128, h-y))

    if edge_tasks:
        tiles = np.array([get_bitmask(frame[t[1]:t[1]+t[3], t[0]:t[0]+t[2]]) for t in edge_tasks], dtype=np.uint64)
        results = np.zeros(len(tiles), dtype=np.int32)
        c_lib.match_batch(_SIGS.ctypes.data_as(ctypes.POINTER(ctypes.c_uint64)), 
                          tiles.ctypes.data_as(ctypes.POINTER(ctypes.c_uint64)), 
                          _NPAGES, len(tiles), results.ctypes.data_as(ctypes.POINTER(ctypes.c_int)))
        for i, task in enumerate(edge_tasks):
            manifest[task[4]][4] = int(results[i])
            
    with open(f"{MANIFEST_DIR}/{frame_idx:04d}.bin", "wb") as f:
        pickle.dump(manifest, f)

def main():
    reg, sigs = pickle.load(open(LIB_CACHE, "rb"))
    popcounts = [np.unpackbits(s.view(np.uint8)).sum() for s in sigs]
    sorted_indices = np.argsort(popcounts)
    b_pool, w_pool = sorted_indices[:100].tolist(), sorted_indices[-100:].tolist()

    cap = cv2.VideoCapture(VIDEO_PATH)
    frames = []
    frame_hashes = {}
    tasks = []
    
    print("--- Phase 1: Temporal Analysis ---")
    idx = 0
    while True:
        ret, frame = cap.read()
        if not ret: break
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        
        # Check if we've seen this exact image before (Temporal Cache)
        f_hash = hashlib.md5(gray).hexdigest()
        if f_hash in frame_hashes:
            # Just copy the existing manifest file
            source = f"{MANIFEST_DIR}/{frame_hashes[f_hash]:04d}.bin"
            target = f"{MANIFEST_DIR}/{idx:04d}.bin"
            if os.path.exists(source):
                os.system(f"cp {source} {target}")
        else:
            frame_hashes[f_hash] = idx
            tasks.append((idx, gray))
        idx += 1
    cap.release()

    print(f"--- Phase 2: Parallel Solving ({len(tasks)} unique frames) ---")
    # Solve only unique frames across all cores
    with Pool(cpu_count(), initializer=init_worker, initargs=(sigs, len(reg), w_pool, b_pool)) as p:
        list(tqdm(p.imap_unordered(solve_frame_parallel, tasks), total=len(tasks)))

if __name__ == "__main__": main()