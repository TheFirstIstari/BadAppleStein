#!/bin/bash
set -e
cd "$(dirname "$0")"

echo "=== Step 1: Build unified binary ==="
make

echo ""
echo "=== Step 2: Generate test library ==="
python3 gen_test_lib.py --pages 100 --seed 42 --output test_lib

echo ""
echo "=== Step 3: Run encode test ==="
./src/badapplestein encode badapple.mp4 test_output.mov \
    --library test_lib \
    --max-frames 10

echo ""
echo "=== Done ==="
