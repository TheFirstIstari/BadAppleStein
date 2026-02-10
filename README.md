# BadAppleStein

BadAppleStein is a multimedia reconstruction project that recreates high-contrast video animations by matching video frames to a library of PDF pages. It combines C and Python components for efficient frame matching and multiple rendering pipelines to generate creative outputs from a PDF library.

## Demo (official YouTube build)

Below is the demonstration of your version of the project (hosted on YouTube). GitHub strips iframe elements from READMEs, so the demo is presented as a clickable thumbnail below which opens the YouTube video.

[![Watch the demo on YouTube](https://img.youtube.com/vi/Ia1wR8HScm0/0.jpg)](https://www.youtube.com/watch?v=Ia1wR8HScm0)

**YouTube demo:** https://www.youtube.com/watch?v=Ia1wR8HScm0

## Overview

- Purpose: Convert high-contrast video frames into an animation assembled from a library of PDF pages so that the video is represented using PDF content.
- Implementation: Core matching routines implemented in C (compiled to a shared object used by Python), with multiple Python scripts for arranging and rendering (`job1_*`, `job2_*`). FFmpeg is used for standard video processing tasks.

## Files of interest

- `match.c`, `libmatch.so` — C matching implementation and compiled shared library.
- `job1_*.py` — arrangement scripts that select and place PDF frames.
- `job2_*.py` — rendering scripts that assemble the final output.
- `library.pkl` — precomputed index or library used for matching (if present).

## Notes

- This README intentionally omits setup and build instructions due to the project's complex dependencies and platform-specific build steps.
- The `badapple.mp4` file present in the repo is the original classic Bad Apple video; the YouTube link above points to your project build.

---

If you'd like, I can also:
- Add a small GIF or a few still-frame thumbnails into the repo and reference them (recommended for smaller repo size).
- Commit and push this change to the remote for you.

Which of those should I do next?cat > "BadAppleStein/README.md" <<'EOF'
# BadAppleStein

BadAppleStein is a multimedia reconstruction project that recreates high-contrast video animations by matching video frames to a library of PDF pages. It combines C and Python components for efficient frame matching and multiple rendering pipelines to generate creative outputs from a PDF library.

## Demo (official YouTube build)

Below is the demonstration of your version of the project (hosted on YouTube). GitHub strips iframe elements from READMEs, so the demo is presented as a clickable thumbnail below which opens the YouTube video.

[![Watch the demo on YouTube](https://img.youtube.com/vi/Ia1wR8HScm0/0.jpg)](https://www.youtube.com/watch?v=Ia1wR8HScm0)

**YouTube demo:** https://www.youtube.com/watch?v=Ia1wR8HScm0

## Overview

- Purpose: Convert high-contrast video frames into an animation assembled from a library of PDF pages so that the video is represented using PDF content.
- Implementation: Core matching routines implemented in C (compiled to a shared object used by Python), with multiple Python scripts for arranging and rendering (`job1_*`, `job2_*`). FFmpeg is used for standard video processing tasks.

## Files of interest

- `match.c`, `libmatch.so` — C matching implementation and compiled shared library.
- `job1_*.py` — arrangement scripts that select and place PDF frames.
- `job2_*.py` — rendering scripts that assemble the final output.
- `library.pkl` — precomputed index or library used for matching (if present).
