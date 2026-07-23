# Audit: Fix arrange.c streaking and bad pattern matching

## Root Cause Analysis

Three bugs in `arrange.c` (and one in `gen_test_lib.py`) explain all the visible artifacts:

### Bug 1: Purity check uses grayscale integral instead of binary integral (STREAKING)

**Location:** `arrange.c` lines 285, 315-319 (in `solve_full`)

The Python reference builds a **binary** integral image (each pixel 0 or 1 after threshold at 127) and checks **exact purity** (`s == area` for all-white, `s == 0` for all-black).

The C code builds a **grayscale** integral (pixel values 0-255), then applies `cnt * 255 / area`. This has two compounding errors:

1. **Formula mismatch:** `cnt` is the sum of grayscale values (not binary 0/1 counts), so `cnt * 255 / area` computes `avg_pixel * 255`, which is always ≫ 147 for any non-trivial block. Nearly every block passes the purity check, so the solver grows everything to max_block.

2. **Fuzzy vs exact:** Even with the correct formula, the ±20 tolerance allows ~40% impurity. Python requires 100% purity (every pixel on the same side of 127).

**Fix:** Threshold the grayscale to binary (using existing `img_threshold_u8`), build integral from that, use exact equality check. This matches the Python behavior exactly.

### Bug 2: Feature quantization truncates instead of rounding (BAD MATCHING)

**Location:** `imgops.c` line 146

The C code uses `v * maxv / 255` with integer truncation. For G=1 (maxv=1):
- v=128 → 128/255 = **0** (should be 1)
- v=254 → 254/255 = **0** (should be 1)  
- v=255 → 255/255 = **1**

Only pure-white pixels register as "on". The original Python thresholded at 127, so everything in the upper half was "on". Features are almost entirely zeros → no discrimination → random matching.

**Fix:** Change to `(v * maxv + 127) / 255` (round to nearest). This gives threshold at ~128, matching Python's threshold-at-127 behavior.

### Bug 3: gen_test_lib.py has same quantization bug

**Location:** `gen_test_lib.py` line 56

`((ch.astype(np.float32) / 255.0) * max_val).astype(np.uint8)` truncates the float to uint8, giving the same broken threshold as the C code. Library and query features are *self-consistent* but both have poor discrimination.

**Fix:** Use `np.round()` before `.astype(np.uint8)`.

### Bug 4: Hero threshold too low (minor)

**Location:** `arrange.c` line 186

Default `hero_min_pct = 0.0625` → hero_min = 24px. Python uses fixed 32px. Blocks 24-31px should be matched, not filled with solid color.

**Fix:** Change default from 0.0625 to 0.0833 (= 32/384, matching Python's 32px).

---

## Implementation Plan

### Step 1: Fix purity check in `arrange.c` `solve_full()`

Replace the grayscale integral with a binary integral:

```c
// After computing gray, create binary version for purity checks
uint8_t *binary = (uint8_t *)malloc((size_t)w * h);
memcpy(binary, gray, (size_t)w * h);
img_threshold_u8(binary, w * h, 127, 1);  // >127 → 1, ≤127 → 0

int64_t *sum = img_integral(binary, w, h);  // Binary integral
free(binary);  // Can free after integral is built
```

Replace the purity check:
```c
// OLD (broken):
int pure = (color == 1) ? ((int)(cnt * 255 / area) >= (127 + 20))
                         : ((int)(cnt * 255 / area) <= (127 - 20));

// NEW (exact, matching Python):
int pure = (color == 1) ? (cnt == area) : (cnt == 0);
```

Also fix the color seed check to use `>` instead of `>=` for consistency with the binary threshold:
```c
// OLD: int color = (gray[(size_t)y * w + x] >= 127) ? 1 : 0;
// NEW: int color = (gray[(size_t)y * w + x] > 127) ? 1 : 0;
```

### Step 2: Fix feature quantization in `imgops.c`

```c
// OLD (line 146):
int q = (G >= 8) ? v : (v * maxv / 255);

// NEW (round to nearest):
int q = (G >= 8) ? v : (v * maxv + 127) / 255;
```

### Step 3: Fix feature quantization in `gen_test_lib.py`

```python
# OLD (line 56):
quantized = ((ch.astype(np.float32) / 255.0) * max_val).astype(np.uint8)

# NEW (round instead of truncate):
quantized = np.round(ch.astype(np.float32) / 255.0 * max_val).astype(np.uint8)
```

### Step 4: Fix hero threshold in `arrange.c`

```c
// OLD (line 23):
static double g_max_block_pct = 0.5, g_hero_min_pct = 0.0625;

// NEW:
static double g_max_block_pct = 0.5, g_hero_min_pct = 0.0833;
```

### Step 5: Regenerate test library and re-run

After all fixes, regenerate the test library (features change due to quantization fix) and re-run the pipeline to verify:
1. No streaking (blocks respect edges)
2. Meaningful pattern matching (features discriminate between pages)
3. Hero blocks at 32px+ only

## Files to modify

| File | Change |
|------|--------|
| `BadAppleStein/arrange.c` | Binary purity check, hero threshold, color seed check |
| `BadAppleStein/imgops.c` | Feature quantization rounding |
| `BadAppleStein/gen_test_lib.py` | Feature quantization rounding |
