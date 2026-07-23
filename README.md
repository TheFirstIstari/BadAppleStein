# BadAppleStein

Reconstructs any video as a mosaic of pages from a source library of PDFs and images. A pure-C pipeline with SIMD-accelerated matching and hardware encoding support.

[![Watch the demo on YouTube](https://img.youtube.com/vi/Ia1wR8HScm0/0.jpg)](https://www.youtube.com/watch?v=Ia1wR8HScm0)

## Installation

### Pre-built binaries

```bash
curl -fsSL https://raw.githubusercontent.com/TheFirstIstari/BadApplestein/master/install.sh | sh
```

### Build from source

Requires: [ffmpeg](https://ffmpeg.org/) (libavformat, libavcodec, libavutil, libswscale), optionally [mupdf](https://mupdf.com/) for PDF support, and [libomp](https://openmp.llvm.org/) for parallel matching.

```bash
git clone https://github.com/TheFirstIstari/BadApplestein.git
cd BadApplestein
make              # builds src/badapplestein
make install      # installs to /usr/local/bin (override with PREFIX=)
```

## Usage

```bash
# Build a source library from images/PDFs
badapplestein build ~/Documents/pdfs/

# Encode a video using the library
badapplestein encode input.mp4 output.mov

# Shorthand (auto-detects encode mode)
badapplestein input.mp4 output.mov

# Use a preset
badapplestein input.mp4 output.mov --preset 4k
```

### Subcommands

| Command | Description |
|---------|-------------|
| `badapplestein build <sources_dir>` | Build a source library from images/PDFs |
| `badapplestein encode <input> <output>` | Encode a video using the library |

### Options

| Flag | Description |
|------|-------------|
| `--library <dir>` | Library directory (default: `./` or `~/.badapplestein/library/`) |
| `--preset <name>` | Output preset: `8k`, `4k`, `1080p`, `720p` |
| `--width <n>` | Output width (overrides preset) |
| `--height <n>` | Output height |
| `--fps <n>` | Output fps |
| `--codec <name>` | Output codec (default: auto-detect best available) |
| `--no-hw` | Disable hardware encoding |
| `--max-frames <n>` | Process only N frames (0 = all) |
| `--threads <n>` | Thread count (0 = auto) |
| `--keep-manifests` | Keep temp manifests after encoding |
| `--verbose`, `-v` | Verbose output |
| `--json` | JSON output mode |

## Pipeline

```
input.mp4 → arrange → manifests/*.bin → render → output.mov
              ↓            ↑
         source lib    (pdf_path, page_idx) per tile
```

## Building

```bash
make              # Build unified binary
make install      # Install to $PREFIX/bin (default: /usr/local)
make clean        # Remove build artifacts
```

### Development (legacy standalone binaries)

```bash
make arrange      # Build arrange-only binary
make render       # Build render-only binary
make build-library
```

## Source Library

The arrange stage matches against a pre-built feature library:

- `features.bin` — Quantized feature vectors (multi-resolution, 1-8 bits/cell, grayscale or color)
- `registry.bin` — Maps library entries to source file paths and page indices

Generate a test library:

```bash
python3 gen_test_lib.py --pages 2000 --seed 42 --output test_lib
```

## Performance

BadAppleStein outputs JSON progress that can be captured for benchmarking:

```bash
# Run with JSON output
./src/badapplestein encode input.mp4 output.mov --json 2> progress.json

# Or use hyperfine for timing
mise run benchmark-arrange
mise run benchmark-render
```

Performance characteristics (v1.0.0):
- Arrange: ~1-5 fps depending on resolution and match quality threshold
- Render: ~10-30 fps with atlas cache, hardware encoding
- Cache hit rate: 60-90% for typical videos (reduces source image loads)

The `--max-frames` option is useful for quick performance testing.

Note: The unified binary handles both arrange and render stages automatically in a single `encode` command; standalone `arrange` and `render` binaries are available for development and debugging.

## Key Files

| File | Description |
|------|-------------|
| `src/main.c` | Unified CLI entry point |
| `src/arrange.c` | Video decode + greedy block solver + feature extraction + matching |
| `src/render.c` | Manifest loading + source page rendering + frame assembly |
| `src/match.c` | L1 feature matcher (SIMD: AVX2/SSE2/NEON, OpenMP-parallel) |
| `src/video.c` | FFmpeg libav decode/encode + image loader |
| `src/imgops.c` | Grayscale conversion, resize, feature extraction |
| `src/pdf.c` | mupdf PDF rasterization + libav image fallback |
| `src/cli.c` | CLI framework (option parsing, progress, JSON output) |
| `src/build_library.c` | Build a source library from images/PDFs |
| `src/vt_prores.m` | macOS VideoToolbox ProRes hardware encoding |

## License

MIT — see [LICENSE](LICENSE).
