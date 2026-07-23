# Plan: Generalize BadAppleStein into a "match any video to any source set" tool

## Goal
Turn the current hard-coded "Bad Apple video → one fixed PDF library" pipeline into a
**general tool**: given ANY input video and ANY set of source images/PDFs, produce a
matched output video composited from those sources, preserving **grayscale/color fidelity**
(not just 1-bit black/white).

## Fidelity model (decided): grayscale/color
The matcher is generalized from 1-bit Hamming to a **multi-level feature signature**:
- Each tile is downsampled to an N×N feature grid. Each cell holds a small fixed-width
  value: for grayscale, G bits of luminance (e.g. 4–8 bits/cell); for color, the same per
  channel. This keeps the fast array-matcher while leaving the 1-bit case as G=1.
- Distance metric becomes **L1 / SSD over the feature grid** (sum of absolute differences),
  replacing `__builtin_popcountll(a ^ b)`. OpenMP parallelism is preserved.
- This subsumes the old 1-bit path (G=1, L1-on-bits == Hamming) so we don't maintain two
  matchers.

## Target interface (proposed)
```
badapple build-library <sources> [--feat N] [--bits G] [--color] [--multi-scale] [-o library.pkl]
badapple arrange <input.mp4> --library library.pkl [--scale 15] [--feat N] [--bits G] [--color] \
        [--max-block PCT] [--hero-min PCT] [--output manifests/]
badapple render  <manifests/> --library library.pkl [--scale 15] [--audio input.mp4] [-o out.mov]
badapple run <input.mp4> --library library.pkl [--scale 15] [-o out.mov]
```
Output resolution = input video resolution × `scale` (correct aspect ratio).

## What already generalizes (keep)
- Manifest binary format (`uint32 n` + 24-byte records, int32 coords) — source-agnostic.
- `registry.bin` (`op_id → (pdf_path, page_idx)`).
- job2 letterbox placement (composites any source at any aspect).
- Disk-only stage decoupling.

## What must change

### A. New: `build_library.py` (the missing piece)
- Ingest a directory tree (or manifest) of **PDFs AND images**; for each PDF page and each
  image, render/normalize to a grayscale (or color) bitmap.
- Compute the feature signature at chosen N×N grid, G bits/cell (color ⇒ per channel),
  optionally emit **multi-scale / rotation** variants as distinct registry entries.
- Emit `library.pkl` (registry, features) for compatibility + `features.bin` + `registry.bin`
  for the C arrange. Feature layout: `n_pages * N*N * (channels) * ceil(G/8)` bytes, with a
  small header recording `N, G, channels` so reader/matcher agree.
- Reuses the same downsample/quantize math as `get_bitmask` so builder and matcher agree.

### B. `match.c` — generalized distance
- `match_batch` takes feature width params (or reads them from a header) and computes
  **L1/SSD** between target and each library feature instead of popcount-of-XOR.
- Keep `#pragma omp parallel for` for multicore. Early-exit only on exact zero distance.
- Signature length becomes `N*N*channels*words` instead of fixed 64; parameterize.

### C. `arrange.cpp` (and `job1` reference) — parameterize + recolor
- `--feat N --bits G --color`: thread feature dims through `get_bitmask` (now a quantizer),
  tile cache, matcher call, and `features.bin` layout.
- Read frame W/H from video; stop assuming 512×384.
- `--max-block PCT` / `--hero-min PCT`: scale block sizes relative to frame dims.
- Uniform-region ("hero") test becomes intensity/mean check (near max ⇒ white hero,
  near min ⇒ black hero) — generalizes to gray/color. Guard libraries lacking extremes.
- `--video/--manifests/--features/--registry` flags (extend existing arg parsing).
- `is_pure` becomes "intensity variance within threshold" using the feature/intensity image.

### D. `job2_greedy_render.py` — resolution + color aware
- Derive W/H from input video aspect × `scale` (replace fixed 512×384 × 15).
- Load source pages as grayscale OR color per `--color`; paste with the same letterbox policy.
- Decouple audio mux from input path (`--audio`).
- Output filename parameterizable.

### E. `mise.toml` / `run.sh` / README
- Replace `prep-lib` with `build-library` (constructs library from sources).
- Add `arrange`/`render`/`run` tasks taking input video + library.
- README documents the general tool.

## Implementation phases
1. **Feature + matcher core**: define feature format (N×N, G bits/cell, optional color);
   rewrite `get_bitmask`→`compute_feature`; rewrite `match.c` to L1/SSD with feature params.
   Unit-test: G=1 reproduces old Hamming on a known tile.
2. **Library builder**: `build_library.py` (PDFs+images, multiscale) → `library.pkl` +
   `features.bin` + `registry.bin`.
3. **Configurable arrange**: wire `--feat/--bits/--color/--max-block/--hero-min`, derive
   dims from video, color-aware heroes/`is_pure`.
4. **Resolution-aware color render**: job2 derives W/H, handles gray/color, decoupled audio.
5. **Unified CLI + mise + README**.
6. **Validation**: build a small mixed library (a few images + one PDF, multiscale), run a
   short color clip end-to-end; compare against old Bad Apple path to confirm no regression
   and that gray/color detail is preserved (spot-check a frame).

## Risks
- **Matcher speed**: L1/SSD over wider features is heavier than popcount; OpenMP + the
  cross-frame tile cache keep it practical. Very large libraries may later need ANN indexing.
- **Feature design**: G bits/cell and N grid size are tuning knobs; start with N=32,G=4
  (grayscale) as a sane default, N=64,G=1 reproduces legacy.
- **Color render cost**: compositing color frames is heavier but fine for offline render.
- **Multi-scale library** inflates n_pages → O(n_pages) per tile; monitor, add ANN later.
