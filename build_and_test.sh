#!/bin/bash
set -e
cd /Users/frobinson/dev/BadApplestein/BadAppleStein

echo "=== Step 1: Build arrange ==="
mise run build-arrange

echo ""
echo "=== Step 2: Generate test library ==="
python3 gen_test_lib.py --pages 100 --seed 42 --output test_lib

echo ""
echo "=== Step 3: Run arrange test ==="
./arrange --video ../badapple.mp4 --features test_lib/features.bin --registry test_lib/registry.bin --manifests manifests_test --max-frames 10

echo ""
echo "=== Done ==="
