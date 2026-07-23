# BadAppleStein — v1.0 Packaging Plan

## Overview

Package the working pipeline as a polished, distributable tool. Unify the CLI, add Homebrew support, create man pages, and write comprehensive documentation.

## Architecture Decisions

### 1. Unified Binary with Subcommands

Replace three separate binaries (`arrange`, `render`, `build_library`) with a single `badapplestein` binary:

```
badapplestein build <sources_dir> [--library <dir>]
badapplestein encode <input_video> <output_video> [--library <dir>]
```

**Why subcommands over auto-detect:**
- Explicit is better than implicit — user knows what operation they're running
- Avoids ambiguity (is a `.mp4` an input video or a library?)
- Standard pattern (git, docker, ffmpeg all use subcommands)

**Default library location:** `~/.badapplestein/library/` (XDG-compliant via `$HOME`)
- Override with `--library <dir>`
- `build` writes to `--library` (default: `~/.badapplestein/library/`)
- `encode` reads from `--library` (default: `~/.badapplestein/library/`)

### 2. Default Encoding Options (Optimal Defaults)

The `encode` subcommand should require ONLY input and output:

```bash
badapplestein encode input.mp4 output.mov
```

Defaults:
| Option | Default | Rationale |
|--------|---------|-----------|
| `--library` | `~/.badapplestein/library/` | Standard location |
| `--width` | auto from source | Preserve original resolution |
| `--height` | auto from source | Compute from aspect ratio |
| `--fps` | auto from source | Via fps.bin sidecar |
| `--codec` | auto (HW ProRes → HW H.264 → SW ProRes) | Best available |
| `--threads` | 0 (auto-detect CPU cores) | Use all cores |
| `--max-frames` | 0 (all) | Process entire video |
| `--no-hw` | false | Hardware encoding ON by default |

### 3. Default Build Options

```bash
badapplestein build ~/Documents/source-pdfs/
```

Defaults:
| Option | Default | Rationale |
|--------|---------|-----------|
| `--library` | `~/.badapplestein/library/` | Standard location |
| `--bits` | 1 | From library header on subsequent runs |
| `--scales` | 32,64,128 | Multi-resolution matching |
| `--threads` | 0 (auto) | Use all cores |

## Implementation Plan

### Phase 1: Unified Binary (C code changes)

**Files to modify:**
- `BadAppleStein/arrange.c` — Extract `main()` logic into `arrange_main(int argc, char **argv)`
- `BadAppleStein/render.c` — Extract `main()` logic into `render_main(int argc, char **argv)`
- `BadAppleStein/build_library.c` — Extract `main()` logic into `build_main(int argc, char **argv)`
- `BadAppleStein/cli.c` / `cli.h` — Add subcommand parsing

**New file:**
- `BadAppleStein/main.c` — Unified entry point with subcommand dispatch

**Subcommand dispatch:**
```c
int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage();
        return 0;
    }
    if (strcmp(argv[1], "build") == 0) return build_main(argc - 1, argv + 1);
    if (strcmp(argv[1], "encode") == 0) return encode_main(argc - 1, argv + 1);
    // Legacy: if no subcommand, check if first arg is a video file → encode mode
    // This preserves backward compatibility with old CLI
    return encode_main(argc, argv);
}
```

**`encode` wraps `arrange` + `render`:**
- Internally calls arrange logic, then render logic
- Passes manifests directory through (temp dir or same as output)
- Cleans up temp manifests after encoding

**Backward compatibility:**
- If invoked as `arrange` or `render` (symlinks or argv[0] check), run legacy mode
- This allows existing scripts to keep working

### Phase 2: Makefile Updates

**Changes:**
- Add `badapplestein` as the primary target
- Keep `arrange` and `render` as legacy symlinks
- Add `build_library` target
- Update `install` to install `badapplestein` + symlinks
- Add `VERSION` variable passed via `-DVERSION`

### Phase 3: Man Pages

**Files to create:**
- `BadAppleStein/man/badapplestein.1` — Main page (overview, examples)
- `BadAppleStein/man/badapplestein-build.1` — Build subcommand
- `BadAppleStein/man/badapplestein-encode.1` — Encode subcommand

**Format:** Standard roff/troff man pages with sections:
- NAME, SYNOPSIS, DESCRIPTION, OPTIONS, EXAMPLES, FILES, SEE ALSO, AUTHORS

**Makefile targets:**
- `make man` — Generates man pages from templates
- `make install` — Installs man pages to `$(PREFIX)/share/man/man1/`

### Phase 4: README

**Structure:**
```
# BadAppleStein

One-line description

## Quick Start
- Install (Homebrew, build from source)
- Build a library
- Encode a video

## Installation
### Homebrew (recommended)
### Pre-built binaries
### Build from source

## Usage
### badapplestein build
### badapplestein encode
### Examples

## Configuration
### Library location
### Hardware encoding
### Threading

## Advanced
### Custom library options
### Manual arrange/render pipeline
### Performance tuning

## Building from Source
### Dependencies
### Build
### Test

## License
```

### Phase 5: Homebrew Formula

**File:** `Formula/badapplestein.rb`

```ruby
class Badapplestein < Formula
  desc "Reconstruct any video using a mosaic of source library pages"
  homepage "https://github.com/frobinson/BadApplestein"
  url "https://github.com/frobinson/BadApplestein/archive/refs/tags/v1.0.0.tar.gz"
  license "MIT"
  depends_on "pkg-config" => :build
  depends_on "ffmpeg"
  depends_on "mupdf" => :optional

  def install
    system "make", "PREFIX=#{prefix}"
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    system "#{bin}/badapplestein", "--help"
  end
end
```

**Tap repo:** `frobinson/homebrew-badapplestein`

### Phase 6: CI Updates

**Changes to `.github/workflows/release.yml`:**
- Build `badapplestein` binary (not just arrange/render)
- Include man pages in tarball
- Add Linux arm64 build
- Smoke test: `badapplestein --help` and `badapplestein build --help`

### Phase 7: Installer Updates

**Changes to `install.sh`:**
- Fix naming: tarball contains `badapplestein` binary + symlinks
- Verify `badapplestein --help` works after install
- Add shell completions (bash/zsh/fish) — optional, future

## File Manifest

### New files
| File | Description |
|------|-------------|
| `BadAppleStein/main.c` | Unified entry point |
| `BadAppleStein/man/badapplestein.1` | Main man page |
| `BadAppleStein/man/badapplestein-build.1` | Build subcommand man page |
| `BadAppleStein/man/badapplestein-encode.1` | Encode subcommand man page |
| `Formula/badapplestein.rb` | Homebrew formula |

### Modified files
| File | Changes |
|------|---------|
| `BadAppleStein/arrange.c` | Extract main → arrange_main |
| `BadAppleStein/render.c` | Extract main → encode_main (wraps arrange+render) |
| `BadAppleStein/build_library.c` | Extract main → build_main |
| `BadAppleStein/cli.c` | Add --library default path logic |
| `BadAppleStein/cli.h` | Add subcommand function declarations |
| `Makefile` | Add badapplestein target, man pages, updated install |
| `README.md` | Complete rewrite |
| `.github/workflows/release.yml` | Updated CI |
| `install.sh` | Fix naming, verify install |

## Execution Order

1. **Phase 1** — Unified binary (C code) — highest priority, core functionality
2. **Phase 2** — Makefile updates — enables building
3. **Phase 3** — Man pages — documentation
4. **Phase 4** — README — documentation
5. **Phase 5** — Homebrew formula — distribution
6. **Phase 6** — CI updates — automated releases
7. **Phase 7** — Installer updates — user-facing

## Open Questions

1. Should `encode` auto-clean the temp manifests directory, or leave it for debugging?
2. Should we support `badapplestein <input> <output>` (no subcommand) as a shortcut for `encode`?
3. Should the library be per-project (in current directory) or global (~/.badapplestein/)?
4. Should we add `--preset` options (e.g., `--preset 8k`, `--preset 4k`) for common workflows?
