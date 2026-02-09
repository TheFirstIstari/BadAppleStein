import cv2
import numpy as np
import pickle
import os
import pypdfium2 as pdfium
from tqdm import tqdm

# --- CONFIG ---
MANIFEST_DIR = "manifests"
LIB_CACHE = "library.pkl"
OUTPUT_VIDEO = "bad_apple_final.mp4"
FRAME_SIZE = (512, 384)
FPS = 30.0

def render_video():
    if not os.path.exists(LIB_CACHE):
        print(f"ERROR: {LIB_CACHE} not found. Run Job 1 first!")
        return

    # Load the registry to know which ID belongs to which PDF
    print("--- Loading Registry ---")
    registry, _ = pickle.load(open(LIB_CACHE, "rb"))
    
    # Setup Video Writer
    fourcc = cv2.VideoWriter_fourcc(*'mp4v')
    out = cv2.VideoWriter(OUTPUT_VIDEO, fourcc, FPS, FRAME_SIZE)

    # Get sorted manifest files
    manifest_files = sorted([f for f in os.listdir(MANIFEST_DIR) if f.endswith(".bin")])
    if not manifest_files:
        print("ERROR: No manifests found in 'manifests/' folder.")
        return

    # This dictionary will store rendered grayscale pages in RAM
    # With 10GB of PDFs, this could grow, but you have high RAM capacity.
    atlas = {}
    
    # Track open PDF documents to avoid overhead of opening/closing
    open_docs = {}

    print(f"--- Rendering {len(manifest_files)} Frames ---")
    for m_file in tqdm(manifest_files, desc="Rendering Video", unit="frame"):
        # Create a blank black canvas for the frame
        canvas = np.zeros((FRAME_SIZE[1], FRAME_SIZE[0]), dtype=np.uint8)
        
        with open(os.path.join(MANIFEST_DIR, m_file), "rb") as f:
            instructions = pickle.load(f)
        
        for x, y, w, h, pdf_id in instructions:
            if pdf_id == -1: # Solid Black (Already black by default)
                continue
            elif pdf_id == -2: # Solid White
                canvas[y:y+h, x:x+w] = 255
            else:
                # If page is not in RAM, render it now
                if pdf_id not in atlas:
                    pdf_path, pg_idx = registry[pdf_id]
                    
                    # Keep PDF document open in memory to speed up access
                    if pdf_path not in open_docs:
                        open_docs[pdf_path] = pdfium.PdfDocument(pdf_path)
                    
                    doc = open_docs[pdf_path]
                    page = doc[pg_idx]
                    
                    # Render at 2x scale for high detail in the tiles
                    bitmap = page.render(scale=2.0).to_numpy()
                    gray_page = cv2.cvtColor(bitmap, cv2.COLOR_BGRA2GRAY)
                    atlas[pdf_id] = gray_page
                
                # Resize the cached PDF page to fit the specific tile
                patch = cv2.resize(atlas[pdf_id], (w, h), interpolation=cv2.INTER_AREA)
                canvas[y:y+h, x:x+w] = patch
        
        # Convert grayscale canvas to BGR for VideoWriter
        final_frame = cv2.cvtColor(canvas, cv2.COLOR_GRAY2BGR)
        out.write(final_frame)

    # Cleanup
    for doc in open_docs.values():
        doc.close()
    out.release()
    print(f"\n--- SUCCESS! Final video saved as {OUTPUT_VIDEO} ---")

if __name__ == "__main__":
    render_video()