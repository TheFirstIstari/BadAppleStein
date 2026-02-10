# BadAppleStein

BadAppleStein is a multimedia reconstruction project that recreates high-contrast video animations by matching video frames to a library of PDF pages. It combines C and Python components for efficient frame matching and multiple rendering pipelines to generate creative outputs from a PDF library.

## Demo (official YouTube build)

Below is the demonstration of your version of the project (hosted on YouTube). This is the build you indicated is the correct version — it is embedded directly for convenience.

<div style="max-width:900px;">
  <iframe width="100%" height="506" src="https://www.youtube.com/embed/Ia1wR8HScm0" title="BadAppleStein demo" frameborder="0" allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share" allowfullscreen></iframe>
</div>

**YouTube demo:** https://www.youtube.com/watch?v=Ia1wR8HScm0

## Overview

- Purpose: Convert high-contrast video frames into an animation assembled from a library of PDF pages so that the video is represented using PDF content.
- Implementation: Core matching routines implemented in C (compiled to a shared object used by Python), with multiple Python scripts for arranging and rendering (`job1_*`, `job2_*`). FFmpeg is used for standard video processing tasks.

## How it works (high level)

1. Decode the input video into frames (FFmpeg or other tooling).
2. For each frame, compute a compact representation and search the PDF library for the best-matching page(s) using the optimized C matching routines.
3. Arrange the selected PDF pages into a render timeline and post-process into a final video using the provided Python scripts.

## Files of interest

- `match.c`, `libmatch.so` — C matching implementation and compiled shared library.
- `job1_*.py` — arrangement scripts that select and place PDF frames.
- `job2_*.py` — rendering scripts that assemble the final output.
- `library.pkl` — precomputed index or library used for matching (if present).

## Notes

- This README intentionally omits setup and build instructions due to the project's complex dependencies and platform-specific build steps.
- The `badapple.mp4` file present in the repo is the original classic Bad Apple video; the YouTube embed above points to your project build.

---

If you'd like, I can add a gallery of still frames or a small GIF for the README (smaller files are generally friendlier on GitHub). Let me know and I'll add them.


