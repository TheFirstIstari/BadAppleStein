# BadAppleStein

Reconstructs high-contrast video animations by matching video frames to a library of source images/PDFs. A pure-C pipeline: arrange (tile matching) → render (video assembly).

[![Watch the demo on YouTube](https://img.youtube.com/vi/Ia1wR8HScm0/0.jpg)](https://www.youtube.com/watch?v=Ia1wR8HScm0)

## Quick Start

```bash
# Build and run the full pipeline
mise run run

# Or run stages separately
mise run arrange   # Stage 1: Match video tiles to source library
mise run render    # Stage 2: Render final video

# Test with a small sample
mise run test
```

## Pipeline

```
input.mp4 → arrange → manifests/*.bin → render → output.mov
              ↓            ↑
         source lib    (pdf_path, page_idx) per tile
```

| Stage | Binary | Description |
|-------|--------|-------------|
| **arrange** | `arrange` | Decodes video, extracts features, matches tiles to library entries, writes binary manifests |
| **render** | `render` | Reads manifests, renders source pages (PDF/images via mupdf + libav), assembles output video |

## Building

Requires: [ffmpeg](https://ffmpeg.org/) (libavformat, libavcodec, libavutil, libswscale), optionally [mupdf](https://mupdf.com/) for PDF support.

```bash
mise run build-arrange   # Compile the arrange engine
mise run build-render    # Compile the render engine (needs mupdf for PDFs)
```

## Source Library

The arrange stage matches against a pre-built feature library:

- `features.bin` — Quantized feature vectors (N×N grid, 1-8 bits/cell, grayscale or color)
- `registry.bin` — Maps library entries to source file paths and page indices

Generate a test library:

```bash
mise run gen-lib   # Creates test_lib/ with 200 random PNG pages
```

## Key Files

| File | Description |
|------|-------------|
| `arrange.c` | Video decode + greedy block solver + feature extraction + matching |
| `render.c` | Manifest loading + source page rendering + frame assembly |
| `match.c` | L1 feature matcher (OpenMP-parallel, pages-outer for cache locality) |
| `video.c` | FFmpeg libav decode/encode + image loader |
| `imgops.c` | Grayscale conversion, resize, integral image, feature extraction |
| `pdf.c` | mupdf PDF rasterization + libav image fallback |
| `cli.c` | CLI framework (option parsing, progress, JSON output) |
| `build_library.c` | Build a source library from images/PDFs |
| `gen_test_lib.py` | Generate test libraries with random features |

## Configuration

```bash
# Override defaults via environment or flags
mise run arrange -- --video myvideo.mp4 --max-frames 100 --verbose
mise run render -- --output result.mov --width 1920 --height 1080 --fps 30
```

## License

See repository for license details.
