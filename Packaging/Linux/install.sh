#!/bin/sh
# Rux per-user installer for Linux.
#
# Downloads a Rux release from GitHub, installs the `rux` binary into a
# per-user directory (default ~/.local/bin, no root required), and makes sure
# that directory is on your PATH.
#
# Usage:
#   curl -fsSL https://rux-lang.dev/install.sh | sh
#
# Or download and run with options:
#   ./install.sh [--version X.Y.Z] [--dir DIR] [--no-modify-path]
#
# Options (also settable via environment variables):
#   --version X.Y.Z     Install a specific version       (RUX_VERSION; default: latest)
#   --dir DIR           Install directory                (RUX_INSTALL_DIR; default: ~/.local/bin)
#   --no-modify-path    Do not touch any shell rc files  (RUX_NO_MODIFY_PATH=1)
#   --help              Show this help and exit
set -eu

REPO="rux-lang/Rux"
ASSET="rux-linux.tar.gz"
BIN="rux"

# --- Defaults (overridable by env, then by flags) -------------------------
VERSION="${RUX_VERSION:-}"
INSTALL_DIR="${RUX_INSTALL_DIR:-$HOME/.local/bin}"
NO_MODIFY_PATH="${RUX_NO_MODIFY_PATH:-}"

# --- Pretty output --------------------------------------------------------
if [ -t 1 ]; then
    BOLD="$(printf '\033[1m')"; RED="$(printf '\033[31m')"
    GREEN="$(printf '\033[32m')"; YELLOW="$(printf '\033[33m')"
    RESET="$(printf '\033[0m')"
else
    BOLD=""; RED=""; GREEN=""; YELLOW=""; RESET=""
fi
info()  { printf '%s\n' "${BOLD}$*${RESET}"; }
warn()  { printf '%s\n' "${YELLOW}warning:${RESET} $*" >&2; }
err()   { printf '%s\n' "${RED}error:${RESET} $*" >&2; exit 1; }

usage() {
    sed -n '2,/^set -eu/{/^set -eu/d;s/^# \{0,1\}//;p;}' "$0" 2>/dev/null || true
    exit "${1:-0}"
}

# --- Parse arguments ------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="${2:-}"; shift 2 ;;
        --version=*) VERSION="${1#*=}"; shift ;;
        --dir) INSTALL_DIR="${2:-}"; shift 2 ;;
        --dir=*) INSTALL_DIR="${1#*=}"; shift ;;
        --no-modify-path) NO_MODIFY_PATH=1; shift ;;
        -h|--help) usage 0 ;;
        *) err "unknown option: $1 (use --help)" ;;
    esac
done

# --- Sanity checks --------------------------------------------------------
arch="$(uname -m)"
case "$arch" in
    x86_64|amd64) : ;;
    *) err "unsupported architecture '$arch'. Rux currently ships an x86_64
       Linux binary only. Build from source: https://github.com/$REPO" ;;
esac

if command -v curl >/dev/null 2>&1; then
    dl() { curl -fsSL "$1" -o "$2"; }
elif command -v wget >/dev/null 2>&1; then
    dl() { wget -qO "$2" "$1"; }
else
    err "neither curl nor wget found; install one and re-run."
fi

# --- Resolve the download URL ---------------------------------------------
if [ -n "$VERSION" ]; then
    VERSION="${VERSION#v}"   # accept either "0.3.0" or "v0.3.0"
    URL="https://github.com/$REPO/releases/download/v$VERSION/$ASSET"
    info "Installing Rux v$VERSION"
else
    URL="https://github.com/$REPO/releases/latest/download/$ASSET"
    info "Installing the latest Rux release"
fi

# --- Download and unpack into a temp dir ----------------------------------
tmp="$(mktemp -d "${TMPDIR:-/tmp}/rux-install.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT INT TERM

info "Downloading $ASSET..."
if ! dl "$URL" "$tmp/$ASSET"; then
    err "download failed: $URL
       Check the version exists at https://github.com/$REPO/releases"
fi

info "Extracting..."
tar -xzf "$tmp/$ASSET" -C "$tmp" || err "failed to extract $ASSET"
[ -f "$tmp/$BIN" ] || err "archive did not contain '$BIN'"

# --- Install --------------------------------------------------------------
mkdir -p "$INSTALL_DIR" || err "could not create install dir: $INSTALL_DIR"
install -m 0755 "$tmp/$BIN" "$INSTALL_DIR/$BIN" 2>/dev/null \
    || { cp "$tmp/$BIN" "$INSTALL_DIR/$BIN" && chmod 0755 "$INSTALL_DIR/$BIN"; } \
    || err "could not install to $INSTALL_DIR (no write permission?)"

info "${GREEN}Installed $BIN to $INSTALL_DIR/$BIN${RESET}"

# --- Ensure the install dir is on PATH ------------------------------------
on_path=0
case ":$PATH:" in *":$INSTALL_DIR:"*) on_path=1 ;; esac

if [ "$on_path" -eq 1 ]; then
    : # already reachable
elif [ -n "$NO_MODIFY_PATH" ]; then
    warn "$INSTALL_DIR is not on your PATH. Add it manually:
       export PATH=\"$INSTALL_DIR:\$PATH\""
else
    # Pick the rc file for the user's login shell; default to ~/.profile.
    shell_name="$(basename "${SHELL:-sh}")"
    case "$shell_name" in
        bash) rc="$HOME/.bashrc" ;;
        zsh)  rc="${ZDOTDIR:-$HOME}/.zshrc" ;;
        fish) rc="$HOME/.config/fish/config.fish" ;;
        *)    rc="$HOME/.profile" ;;
    esac

    line="export PATH=\"$INSTALL_DIR:\$PATH\""
    [ "$shell_name" = "fish" ] && line="fish_add_path \"$INSTALL_DIR\""

    if [ -f "$rc" ] && grep -Fq "$INSTALL_DIR" "$rc" 2>/dev/null; then
        : # an entry already references the dir
    else
        mkdir -p "$(dirname "$rc")"
        {
            printf '\n# Added by the Rux installer\n'
            printf '%s\n' "$line"
        } >> "$rc"
        info "Added $INSTALL_DIR to PATH in $rc"
    fi
    warn "Open a new terminal (or run: ${BOLD}. \"$rc\"${RESET}) so PATH takes effect."
fi

# --- Done -----------------------------------------------------------------
info "${GREEN}Done.${RESET} Get started with:"
printf '    %s help\n' "$BIN"
