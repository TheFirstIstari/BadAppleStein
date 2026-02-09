import os, cv2, numpy as np, pypdfium2 as pdfium, ctypes, pickle
from tqdm import tqdm
from concurrent.futures import ProcessPoolExecutor

# --- CONFIG ---
PDF_ROOT = "Epstein"          
VIDEO_PATH = "badapple.mp4"   
MANIFEST_DIR = "manifests_greedy"
LIB_CACHE = "library.pkl"
LIB_PATH = "./libmatch.so"

# SETTINGS FOR OPTIMAL FILL
MIN_BLOCK = 16   # Smallest detail for silhouettes
MAX_BLOCK = 256  # Largest possible PDF page (Backgrounds)

os.makedirs(MANIFEST_DIR, exist_ok=True)

c_lib = ctypes.CDLL(LIB_PATH)
c_lib.match_batch.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64), 
                               ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]

def get_bitmask(img):
    resized = cv2.resize(img, (64, 64), interpolation=cv2.INTER_AREA)
    _, binary = cv2.threshold(resized, 127, 255, cv2.THRESH_BINARY)
    return np.packbits(binary).view(np.uint64)

def solve_greedy_accurate(frame, signatures, n_pages, pid_white, pid_black):
    h, w = frame.shape
    _, binary = cv2.threshold(frame, 127, 1, cv2.THRESH_BINARY)
    sum_table = cv2.integral(binary.astype(np.uint8))
    visited = np.zeros_like(binary, dtype=bool)
    manifest, tiles_to_match, placeholders = [], [], []

    def is_pure(x, y, rw, rh, color):
        s = sum_table[y+rh, x+rw] - sum_table[y, x+rw] - sum_table[y+rh, x] + sum_table[y, x]
        return s == rw * rh if color == 1 else s == 0

    # Step through the frame
    for y in range(0, h, 8):
        for x in range(0, w, 8):
            if visited[y, x]: continue
            color = binary[y, x]
            mw, mh = 8, 8
            
            # 1. Grow Rectangle greedily up to MAX_BLOCK
            while x + mw + 8 <= w and mw + 8 <= MAX_BLOCK:
                if not visited[y:y+mh, x+mw:x+mw+8].any() and is_pure(x, y, mw+8, mh, color):
                    mw += 8
                else: break
            while y + mh + 8 <= h and mh + 8 <= MAX_BLOCK:
                if not visited[y+mh:y+mh+8, x:x+mw].any() and is_pure(x, y, mw, mh+8, color):
                    mh += 8
                else: break
            
            visited[y:y+mh, x:x+mw] = True
            
            # 2. Assign PDF ID
            if mw >= 32 and mh >= 32:
                # LARGE BLOCK: Use the pre-calculated 'Hero' PDFs
                # Instead of solid color -1/-2, we use actual PDF IDs
                manifest.append([x, y, mw, mh, pid_white if color == 1 else pid_black])
            else:
                # EDGE BLOCK: Needs visual pattern matching
                tiles_to_match.append(get_bitmask(frame[y:y+mh, x:x+mw]))
                placeholders.append(len(manifest))
                manifest.append([x, y, mw, mh, None])

    # 3. Batch Match the detail tiles
    if tiles_to_match:
        results = np.zeros(len(tiles_to_match), dtype=np.int32)
        batch = np.array(tiles_to_match, dtype=np.uint64)
        c_lib.match_batch(signatures.ctypes.data_as(ctypes.POINTER(ctypes.c_uint64)), 
                          batch.ctypes.data_as(ctypes.POINTER(ctypes.c_uint64)), 
                          n_pages, len(batch), 
                          results.ctypes.data_as(ctypes.POINTER(ctypes.c_int)))
        for i, idx in enumerate(placeholders):
            manifest[idx][4] = int(results[i])
            
    return manifest

def main():
    registry, signatures = pickle.load(open(LIB_CACHE, "rb"))
    
    # FIND THE "HERO" PDFs (Whitest and Blackest)
    # We do this by summing the bitmasks. 
    # High popcount = White/Complex, Low popcount = Black
    popcounts = [np.unpackbits(s.view(np.uint8)).sum() for s in signatures]
    pid_white = np.argmax(popcounts)
    pid_black = np.argmin(popcounts)
    print(f"Hero PDFs identified - White ID: {pid_white}, Black ID: {pid_black}")

    cap = cv2.VideoCapture(VIDEO_PATH)
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

    for i in tqdm(range(total), desc="Dynamic Arranging"):
        ret, frame = cap.read()
        if not ret: break
        
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        # Solve with Hero PDF assignment for big areas
        m = solve_greedy_accurate(gray, signatures, len(registry), pid_white, pid_black)
        
        with open(f"{MANIFEST_DIR}/{i:04d}.bin", "wb") as f:
            pickle.dump(m, f)
    cap.release()

if __name__ == "__main__":
    main()