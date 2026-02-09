import os, cv2, numpy as np, pickle
from tqdm import tqdm

# --- CONFIG ---
ATLAS_DIR = "atlas_cache_ultra" # Matches your screenshot
BINARY_ATLAS = "atlas_blob.bin"
IMG_SIZE = 2048 # Standardized high-res size for the blob

def main():
    # Load IDs from the existing cache folder
    unique_ids = sorted([int(f.split('.')[0]) for f in os.listdir(ATLAS_DIR) if f.endswith('.png')])
    total_pages = max(unique_ids) + 1
    
    # Pre-allocate 16GB file on disk (Memory Mapped)
    blob = np.memmap(BINARY_ATLAS, dtype='uint8', mode='w+', shape=(total_pages, IMG_SIZE, IMG_SIZE))

    print(f"--- Packing {len(unique_ids)} PNGs into Binary Blob ---")
    for pid in tqdm(unique_ids):
        img = cv2.imread(f"{ATLAS_DIR}/{pid}.png", cv2.IMREAD_GRAYSCALE)
        if img is None: continue
        # Resize to standard blob size for the master file
        resized = cv2.resize(img, (IMG_SIZE, IMG_SIZE), interpolation=cv2.INTER_AREA)
        blob[pid] = resized
    
    blob.flush()
    print(f"Done! Blob saved as {BINARY_ATLAS}")

if __name__ == "__main__": main()