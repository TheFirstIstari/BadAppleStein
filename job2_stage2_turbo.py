import os, cv2, numpy as np, pickle, subprocess
from tqdm import tqdm
from multiprocessing import Pool, cpu_count

# --- CONFIG ---
MANIFEST_DIR = "manifests_greedy"
BINARY_ATLAS = "atlas_blob.bin"
ORIGINAL_VIDEO = "badapple.mp4"
OUTPUT_MOV = "Bad_Apple_Dynamic.mov"

SCALE_FACTOR = 16 
W, H = 512 * SCALE_FACTOR, 384 * SCALE_FACTOR
IMG_SIZE = 2048 

_BLOB = None

def worker_init(blob_path, total_pages):
    global _BLOB
    # Each core maps the SAME 16GB file. 0 RAM overhead.
    _BLOB = np.memmap(blob_path, dtype='uint8', mode='r', shape=(total_pages, IMG_SIZE, IMG_SIZE))

def render_frame(m_file):
    # Grayscale 16K Canvas
    canvas = np.full((H, W), 255, dtype=np.uint8)
    with open(os.path.join(MANIFEST_DIR, m_file), "rb") as f:
        instructions = pickle.load(f)
    
    for x, y, w, h, pid in instructions:
        nx, ny, nw, nh = x*SCALE_FACTOR, y*SCALE_FACTOR, w*SCALE_FACTOR, h*SCALE_FACTOR
        if pid == -1: canvas[ny:ny+nh, nx:nx+nw] = 0
        elif pid == -2: canvas[ny:ny+nh, nx:nx+nw] = 255
        else:
            img = _BLOB[pid]
            # Fast aspect-fit logic
            ih, iw = img.shape
            as_src, as_tar = iw/ih, nw/nh
            tw, th = (nw, int(nw/as_src)) if as_src > as_tar else (int(nh*as_src), nh)
            
            # Use INTER_AREA for maximum speed at 16K
            resized = cv2.resize(img, (max(1,tw), max(1,th)), interpolation=cv2.INTER_AREA)
            y_off, x_off = (nh-th)//2, (nw-tw)//2
            canvas[ny+y_off:ny+y_off+th, nx+x_off:nx+x_off+tw] = resized
            
    return canvas.tobytes()

def main():
    if not os.path.exists(BINARY_ATLAS):
        print("Run the Packer script first!"); return

    file_size = os.path.getsize(BINARY_ATLAS)
    total_pages = file_size // (IMG_SIZE * IMG_SIZE)
    manifests = sorted([f for f in os.listdir(MANIFEST_DIR) if f.endswith(".bin")])

    # Turbo FFmpeg Settings
    cmd = [
        'ffmpeg', '-y', '-framerate', '30', '-f', 'rawvideo', '-pix_fmt', 'gray',
        '-s', f'{W}x{H}', '-i', '-', '-r', '60', 
        '-c:v', 'prores_ks', '-profile:v', '2', '-vendor', 'apl0', 
        '-pix_fmt', 'yuv422p10le', '-movflags', '+faststart', '-fps_mode', 'cfr', 
        '-threads', 'auto', 'temp_master.mov'
    ]
    # Huge buffer (500MB) ensures Python never waits for FFmpeg
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, bufsize=5 * 10**8)

    # Use 16-20 workers to leave room for FFmpeg and OS
    num_workers = max(1, int(cpu_count() * 0.75))
    print(f"--- Assembling 16K with {num_workers} workers ---")
    
    with Pool(num_workers, initializer=worker_init, initargs=(BINARY_ATLAS, total_pages)) as p:
        # chunksize=1 is important for order in imap
        for frame_bytes in tqdm(p.imap(render_frame, manifests), total=len(manifests)):
            proc.stdin.write(frame_bytes)

    proc.stdin.close(); proc.wait()
    
    # Sync Audio
    subprocess.run(['ffmpeg', '-y', '-i', 'temp_master.mov', '-i', ORIGINAL_VIDEO,
                    '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'copy', '-c:a', 'aac', '-shortest', 'BAD_APPLE_16K_ULTRA_FINAL.mov'])

if __name__ == "__main__": main()