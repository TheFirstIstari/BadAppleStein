# badapplestein

A compact working area for SteinLine-related experiments, helpers and tooling: Python job scripts for arrangement/rendering, small C utilities, and assorted support files used while developing and analyzing SteinLine layouts.

**Project Overview**
- **Purpose:** Collection of tools and experiment scripts used while developing layout/arrangement algorithms and renderers for the SteinLine project.
- **Languages:** Python (primary), C (small utilities / performance-critical parts), HTML/JS (frontend in sibling folders).

**Repository Layout (key files in this folder)**
- `job1_arrange.py`, `job1_greedy_arrange.py`, `job1_hyper_arrange.py` : arrangement experiment scripts.
- `job2_render.py`, `job2_renderfast.py`, `job2_greedy_render.py` : rendering experiments.
- `job2_stage1_pack.py`, `job2_stage2_turbo.py` : packing / pipeline stage scripts.
- `dedupe`, `dedupe.c`, `match.c` : small C utilities / tools used by experiments.
- `last_project.json` : local project snapshot (ignored by git).

**Quick Start**

Prerequisites: Python 3.8+ and a C toolchain (gcc/clang) for building utilities.

1. Create and activate a virtual environment:

```bash
python3 -m venv .venv
source .venv/bin/activate
```

2. Install Python dependencies (if any). There is a top-level `requirements.txt` in the workspace; some subfolders may have their own requirements.

```bash
# from this folder or repo root
pip install -r requirements.txt || true
```

3. Run an example arrangement script:

```bash
python3 job1_arrange.py
```

4. Build C utilities (if needed):

```bash
gcc -O2 dedupe.c -o dedupe
gcc -O2 match.c -o match
```

**Development notes**
- Many scripts are quick experiments — check headers and `--help` for script-specific options.
- Avoid committing large binary/model files; this repo's `.gitignore` already excludes common artifacts (virtual envs, model caches, node_modules, build artifacts, etc.).

**Where to look next**
- The main application and broader project live in sibling folders: `steinline/`, `stein-server/`, `stein-line/` and `Project-SteinLine/`.

**License**
- See the repository `LICENSE` at the workspace root for license terms.

**Contact / Maintainer**
- See top-level project metadata or reach the workspace owner for more context on running experiments and expected environments.
