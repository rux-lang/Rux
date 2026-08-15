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
BIN="rux"

# --- Defaults (overridable by env, then by flags) -------------------------
VERSION="${RUX_VERSION:-}"
INSTALL_DIR="${RUX_INSTALL_DIR:-$HOME/.local/bin}"
NO_MODIFY_PATH="${RUX_NO_MODIFY_PATH:-}"

# --- Pretty output --------------------------------------------------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-}" != dumb ]; then
    BOLD="$(printf '\033[1m')"; RED="$(printf '\033[31m')"
    GREEN="$(printf '\033[32m')"; YELLOW="$(printf '\033[33m')"
    RESET="$(printf '\033[0m')"
else
    BOLD=""; RED=""; GREEN=""; YELLOW=""; RESET=""
fi
info() { printf '%s\n' "${BOLD}$*${RESET}"; }
warn() { printf '%s\n' "${YELLOW}warning:${RESET} $*" >&2; }
err()  { printf '%s\n' "${RED}error:${RESET} $*" >&2; exit 1; }
help() { printf '  help: %s\n' "$*" >&2; }
duration() {
    elapsed=$1
    if [ "$elapsed" -le 0 ]; then
        printf '0 ms'
    elif [ "$elapsed" -lt 60 ]; then
        printf '%s s' "$elapsed"
    else
        printf '%s min %s.0 s' "$((elapsed / 60))" "$((elapsed % 60))"
    fi
}

usage() {
    sed -n '2,/^set -eu/{/^set -eu/d;s/^# \{0,1\}//;p;}' "$0" 2>/dev/null || true
    exit "${1:-0}"
}

# --- Parse arguments ------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --version) [ "$#" -ge 2 ] || err "option '--version' requires a value"; VERSION=$2; shift 2 ;;
        --version=*) VERSION="${1#*=}"; shift ;;
        --dir) [ "$#" -ge 2 ] || err "option '--dir' requires a value"; INSTALL_DIR=$2; shift 2 ;;
        --dir=*) INSTALL_DIR="${1#*=}"; shift ;;
        --no-modify-path) NO_MODIFY_PATH=1; shift ;;
        -h|--help) usage 0 ;;
        *) err "unknown option '$1'; use '--help' to list supported options" ;;
    esac
done

# --- Sanity checks --------------------------------------------------------
arch="$(uname -m)"
case "$arch" in
    x86_64|amd64) architecture=x86_64 ;;
    aarch64|arm64) architecture=aarch64 ;;
    *)
        printf '%s\n' "${RED}error:${RESET} architecture '$arch' is not supported by the Linux installer" >&2
        help "build Rux from source: 'https://github.com/$REPO'"
        exit 1
        ;;
esac
ASSET="rux-linux-$architecture.tar.gz"

if command -v curl >/dev/null 2>&1; then
    dl() { curl -fsSL "$1" -o "$2"; }
elif command -v wget >/dev/null 2>&1; then
    dl() { wget -qO "$2" "$1"; }
else
    err "required downloader 'curl' or 'wget' was not found; install one and run the installer again"
fi
command -v tar >/dev/null 2>&1 || err "required tool 'tar' was not found; install it and run the installer again"

# --- Resolve the download URL ---------------------------------------------
if [ -n "$VERSION" ]; then
    VERSION="${VERSION#v}"   # accept either "0.3.0" or "v0.3.0"
    URL="https://github.com/$REPO/releases/download/v$VERSION/$ASSET"
    info "Installing Rux v$VERSION (linux-$architecture)"
else
    URL="https://github.com/$REPO/releases/latest/download/$ASSET"
    info "Installing latest Rux release (linux-$architecture)"
fi
started_at=$(date +%s)

# --- Download and unpack into a temp dir ----------------------------------
tmp="$(mktemp -d "${TMPDIR:-/tmp}/rux-install.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT INT TERM

info "Downloading '$URL'"
if ! dl "$URL" "$tmp/$ASSET"; then
    printf '%s\n' "${RED}error:${RESET} failed to download '$URL'" >&2
    help "download the release manually from 'https://github.com/$REPO/releases'"
    exit 1
fi

info "Verifying '$ASSET'"
if ! tar -tzf "$tmp/$ASSET" >/dev/null 2>&1; then
    printf '%s\n' "${RED}error:${RESET} downloaded archive from '$URL' is not a valid gzip archive" >&2
    help "remove the download and run the installer again"
    exit 1
fi

info "Extracting '$ASSET'"
if ! tar -xzf "$tmp/$ASSET" -C "$tmp"; then
    printf '%s\n' "${RED}error:${RESET} failed to extract '$tmp/$ASSET'" >&2
    help "extract the archive manually and copy '$BIN' to '$INSTALL_DIR'"
    exit 1
fi
if [ ! -f "$tmp/$BIN" ]; then
    printf '%s\n' "${RED}error:${RESET} archive '$ASSET' does not contain '$BIN'" >&2
    help "download the release manually from 'https://github.com/$REPO/releases'"
    exit 1
fi

# --- Install --------------------------------------------------------------
info "Installing to '$INSTALL_DIR/$BIN'"
mkdir -p "$INSTALL_DIR" || err "failed to create install directory '$INSTALL_DIR'"
install -m 0755 "$tmp/$BIN" "$INSTALL_DIR/$BIN" 2>/dev/null \
    || { cp "$tmp/$BIN" "$INSTALL_DIR/$BIN" && chmod 0755 "$INSTALL_DIR/$BIN"; } \
    || err "failed to install '$BIN' to '$INSTALL_DIR'; check that the destination is writable"

# --- Ensure the install dir is on PATH ------------------------------------
on_path=0
case ":$PATH:" in *":$INSTALL_DIR:"*) on_path=1 ;; esac

if [ "$on_path" -eq 1 ]; then
    info "PATH already contains '$INSTALL_DIR'"
elif [ -n "$NO_MODIFY_PATH" ]; then
    warn "install directory '$INSTALL_DIR' is not on PATH"
    help "add it manually: export PATH=\"$INSTALL_DIR:\$PATH\""
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
        mkdir -p "$(dirname "$rc")" || err "failed to create shell profile directory '$(dirname "$rc")'"
        {
            printf '\n# Added by the Rux installer\n'
            printf '%s\n' "$line"
        } >> "$rc" || err "failed to update shell profile '$rc'"
        info "Added '$INSTALL_DIR' to PATH in '$rc'"
    fi
    info "Restart your terminal to use 'rux' from PATH"
    info "  Current shell: run '. \"$rc\"'"
fi

# --- Done -----------------------------------------------------------------
elapsed=$(( $(date +%s) - started_at ))
info "${GREEN}Installed Rux in $(duration "$elapsed")${RESET}"
info "  Binary: '$INSTALL_DIR/$BIN'"
info "Run 'rux help' to get started"
