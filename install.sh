#!/bin/sh
# Usage: curl -fsSL https://raw.githubusercontent.com/frobinson/BadApplestein/master/install.sh | sh
#
# Install the unified BadApplestein binary from GitHub Releases.
# Creates backward-compatible symlinks for 'arrange' and 'render'.
#
# Options:
#   VERSION=1.0.0 curl ... | sh          Install a specific version
#   BIN_DIR=/usr/local/bin curl ... | sh  Install to a custom directory
#
set -e

REPO="frobinson/BadApplestein"
BIN_NAME="badapplestein"
DEFAULT_VERSION="latest"
DEFAULT_BIN_DIR="$HOME/.local/bin"

VERSION="${VERSION:-$DEFAULT_VERSION}"
BIN_DIR="${BIN_DIR:-$DEFAULT_BIN_DIR}"

# --- Helpers ---

info() {
  printf '  \033[1;34m>\033[0m %s\n' "$1"
}

warn() {
  printf '  \033[1;33m!\033[0m %s\n' "$1"
}

err() {
  printf '  \033[1;31m!\033[0m %s\n' "$1" >&2
}

die() {
  err "$1"
  exit 1
}

# --- Dependency checks ---

check_deps() {
  missing=""
  optional_missing=""

  if ! command_exists curl; then
    die "curl is required but not installed. Please install curl first."
  fi

  if ! command_exists ffmpeg; then
    missing="ffmpeg"
  fi

  if ! command_exists mutool; then
    optional_missing="mupdf"
  fi

  if [ -n "$missing" ]; then
    warn "Missing required dependency: $missing"
    warn "Please install $missing before using $BIN_NAME."
    warn ""
  fi

  if [ -n "$optional_missing" ]; then
    warn "Optional dependency not found: $optional_missing"
    warn "Some features (PDF support) may not work without it."
    warn ""
  fi
}

command_exists() {
  command -v "$1" >/dev/null 2>&1
}

# --- OS / Arch detection ---

detect_platform() {
  os=""
  arch=""

  case "$(uname -s)" in
    Darwin*)  os="darwin" ;;
    Linux*)   os="linux" ;;
    *)        die "Unsupported OS: $(uname -s). Only macOS (Darwin) and Linux are supported." ;;
  esac

  case "$(uname -m)" in
    arm64|aarch64)
      arch="arm64"
      ;;
    x86_64|amd64)
      arch="x86_64"
      ;;
    *)
      die "Unsupported architecture: $(uname -m). Only arm64 and x86_64 are supported."
      ;;
  esac
}

# --- Resolve version ---

resolve_version() {
  if [ "$VERSION" = "latest" ]; then
    info "Resolving latest version..."
    VERSION=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p')
    if [ -z "$VERSION" ]; then
      die "Failed to resolve latest version from GitHub API."
    fi
  fi
  info "Version: $VERSION"
}

# --- Download & install ---

download_and_install() {
  tarball="${BIN_NAME}-${os}-${arch}.tar.gz"
  url="https://github.com/$REPO/releases/download/${VERSION}/${tarball}"

  tmpdir=$(mktemp -d)
  trap 'rm -rf "$tmpdir"' EXIT

  info "Downloading ${tarball}..."
  if ! curl -fSL -o "${tmpdir}/${tarball}" "$url"; then
    die "Download failed. Check that version '$VERSION' exists and has a release for ${os}-${arch}.\n       URL: $url"
  fi

  info "Extracting..."
  tar -xzf "${tmpdir}/${tarball}" -C "$tmpdir"

  info "Installing to ${BIN_DIR}..."
  mkdir -p "$BIN_DIR"

  # Install unified binary
  src="$tmpdir/$BIN_NAME"
  if [ -f "$src" ]; then
    cp "$src" "$BIN_DIR/$BIN_NAME"
    chmod +x "$BIN_DIR/$BIN_NAME"
    info "  installed $BIN_NAME"
  else
    die "$BIN_NAME binary not found in archive"
  fi

  # Create backward-compatible symlinks
  for link_name in arrange render; do
    ln -sf "$BIN_NAME" "$BIN_DIR/$link_name"
    info "  linked $link_name -> $BIN_NAME"
  done

  # Install man pages (if present)
  if [ -d "$tmpdir/man" ]; then
    MAN_DIR="${MAN_DIR:-$HOME/.local/share/man/man1}"
    mkdir -p "$MAN_DIR"
    cp "$tmpdir/man/"*.1 "$MAN_DIR/" 2>/dev/null && info "  installed man pages to $MAN_DIR" || true
  fi

  # Verify installation
  if "$BIN_DIR/$BIN_NAME" --help >/dev/null 2>&1; then
    info "  verified: $BIN_NAME --help works"
  else
    warn "  could not verify $BIN_NAME (may need runtime dependencies)"
  fi
}

# --- PATH check ---

check_path() {
  case ":$PATH:" in
    *":$BIN_DIR:"*) return 0 ;;
  esac
  return 1
}

# --- Main ---

main() {
  info "BadApplestein installer"
  info ""

  detect_platform
  info "Platform: ${os}-${arch}"

  check_deps
  resolve_version
  download_and_install

  info ""
  info "Installation complete!"
  info ""

  # Show installed version
  if "$BIN_DIR/$BIN_NAME" --help >/dev/null 2>&1; then
    info "Installed version:"
    "$BIN_DIR/$BIN_NAME" --help 2>&1 | head -1 || true
    info ""
  fi

  if ! check_path; then
    warn "$BIN_DIR is not in your PATH."
    warn "Add it to your shell profile:"
    warn ""
    warn "  export PATH=\"$BIN_DIR:\$PATH\""
    warn ""
  fi

  info "Usage:"
  info "  $BIN_NAME --help"
  info ""
  info "Documentation:"
  info "  https://github.com/$REPO"
}

main
