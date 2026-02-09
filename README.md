gcc -O2 match.c -o match
# badapplestein

Standalone experiments and utilities for layout arrangement and rendering.

This folder is an independent project: the Python scripts and C utilities here are self-contained and are not tied to other workspace folders. Use this repository for quick experiments, algorithm prototypes, and small performance tools.

Project highlights
- Purpose: run and iterate arrangement/render experiments and tiny C helpers.
- Languages: Python (experiments & orchestration) and C (small, fast utilities).
- Status: experimental — many scripts are prototypes meant for local runs.

Repository layout (important files)
- `job1_arrange.py`, `job1_greedy_arrange.py`, `job1_hyper_arrange.py` — arrangement experiment scripts.
- `job2_render.py`, `job2_renderfast.py`, `job2_greedy_render.py` — rendering experiments.
- `job2_stage1_pack.py`, `job2_stage2_turbo.py` — packing / pipeline stages.
- `dedupe`, `dedupe.c`, `match.c` — small C utilities; source and optionally built binaries.
- `last_project.json` — local snapshot used by scripts (already ignored by git).

Quick start

1) Create and activate a Python virtual environment:

```bash
python3 -m venv .venv
source .venv/bin/activate
```

2) (Optional) Install dependencies if a `requirements.txt` is present for your use-case.

```bash
pip install -r requirements.txt || true
```

3) Build the C utilities (if you need the native binaries):

```bash
gcc -O2 dedupe.c -o dedupe
gcc -O2 match.c -o match
```

4) Run a sample script:

```bash
python3 job1_arrange.py
```

Conventions & notes
- Scripts are experimental — read the top-of-file usage comments and flags.
- `last_project.json` and model caches are intentionally ignored in `.gitignore`.
- Avoid committing large models, datasets, or build artifacts.

Troubleshooting
- If a script fails with missing dependency errors, check for a `requirements.txt` in the repo root or inspect the script imports and install the required packages into the active virtualenv.

License
- See the workspace `LICENSE` for terms.

Maintainer
- Contact the workspace owner for questions about intended experiment inputs, expected outputs, or integration with other tools.
