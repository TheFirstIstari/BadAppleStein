#!/bin/bash
cd /Users/frobinson/dev/BadApplestein

echo "=== Git Commit History ==="
git log --all --oneline 2>&1 | head -50

echo ""
echo "=== Searching for deleted Python files ==="
git log --all --diff-filter=D --name-only -- '*.py' 2>&1

echo ""
echo "=== Trying commit b49aeba ==="
for f in job1_greedy_arrange.py job1_arrange.py job2_greedy_render.py job2_renderfast.py build_library.py; do
  echo "--- $f ---"
  git show "b49aeba:$f" 2>&1
  echo ""
done

echo "=== DONE ==="
