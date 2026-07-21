# BadAppleStein

BadAppleStein is a multimedia reconstruction project that recreates high-contrast video animations by matching video frames to a library of PDF pages. It combines C and Python components for efficient frame matching and multiple rendering pipelines to generate creative outputs from a PDF library.

[![Watch the demo on YouTube](https://img.youtube.com/vi/Ia1wR8HScm0/0.jpg)](https://www.youtube.com/watch?v=Ia1wR8HScm0)

**YouTube demo:** https://www.youtube.com/watch?v=Ia1wR8HScm0

## Quick Start

```bash
# Install dependencies
pip install -r requirements.txt

# Run the full pipeline (arrange + render) - from project root
mise run all

# Or run stages separately
mise run arrange   # Stage 1: Match video tiles to PDF pages
mise run render    # Stage 2: Render final video

# Or use the shell wrapper from BadAppleStein/
cd BadAppleStein && ./run.sh arrange

# Clean generated files
mise run clean-all
```

## Pipeline Stages

| Stage | Script | Description |
|-------|--------|-------------|
| **arrange** | `job1_greedy_arrange.py` | Processes video frames, matches tiles to PDF pages via C matcher, outputs manifests |
| **render** | `job2_greedy_render.py` | Reads manifests, renders cached PDFs, outputs final `.mov` |

## Files of Interest

| File | Description |
|------|-------------|
| `match.c` | Hamming-distance bitmask matcher (C) |
| `libmatch.so` / `libmatch.dylib` | Compiled shared library |
| `job1_*.py` | Arrangement scripts (C matcher integration) |
| `job2_*.py` | Rendering scripts (FFmpeg output) |
| `library.pkl` | Precomputed PDF signatures |
| `mise.toml` | Task runner configuration with perf instrumentation |

## Architecture

```
badapple.mp4 → job1 (C match) → manifests_greedy/*.bin → job2 (render) → output.mov
                ↓                    ↑
            libmatch.so        Self-contained: (pdf_path, page) tuples
```

## Performance Measurement

Both scripts output detailed timing per stage:
- **[PERF] Loading**: Library cache load time
- **[PERF] Stage 1/2/3**: Breakdown of render phases
- **[PERF] arrange complete**: Total time with FPS and tile stats

For more rigorous benchmarking:
```bash
mise run benchmark-arrange
mise run benchmark-render
```

Requires `hyperfine` (`cargo install hyperfine`).