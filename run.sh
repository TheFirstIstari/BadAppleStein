#!/usr/bin/env bash
# BadAppleStein — two-stage pipeline, decoupled by disk.
# Stage 1 (arrange): C engine (libmatch.so/dylib) matches video tiles to PDF
#   pages, writes manifests_greedy/*.bin (self-contained: (pdf_path, page) tuples).
# Stage 2 (render): later, separate — reads manifests_greedy/ alone, emits video.
set -e
cd "$(dirname "$0")"

case "${1:-arrange}" in
  arrange) echo "Stage 1: arrangement (C matcher)"; mise run arrange ;;
  render)   echo "Stage 2: render from manifests_greedy/ (no library.pkl needed)"; mise run render ;;
  all)      echo "Running full pipeline"; mise run run ;;
  bench)      mise run benchmark-arrange; mise run benchmark-render ;;
  clean)      mise run clean-all ;;
  *) echo "usage: $0 [arrange|render|all|bench|clean]"; exit 1 ;;
esac
