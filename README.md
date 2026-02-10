# BadAppleStein

BadAppleStein is a multimedia reconstruction project that recreates high-contrast video animations by matching video frames to a library of PDF pages. It combines C and Python components for efficient frame matching and multiple rendering pipelines to generate creative outputs from a PDF library.


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
