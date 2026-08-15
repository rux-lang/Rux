#!/usr/bin/env sh
# Smoke and message-contract checks for release installers and MSI packaging.
set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
repository_root=$(CDPATH= cd -P "$script_directory/../../.." && pwd)
fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/rux-installer-messages.XXXXXX")
trap 'rm -rf "$fixture_root"' EXIT HUP INT TERM

output=$fixture_root/output.txt
installer=$repository_root/Packaging/Linux/install.sh
mkdir -p "$fixture_root/bin" "$fixture_root/archive"
fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}
require_output() {
    grep -F "$1" "$output" >/dev/null || {
        printf 'error: expected installer output containing: %s\n' "$1" >&2
        sed 's/^/  /' "$output" >&2
        exit 1
    }
}
require_text() {
    grep -F "$2" "$1" >/dev/null || fail "'$1' does not contain required text: $2"
}
run_installer() {
    test_home=$1
    shift
    mkdir -p "$test_home"
    HOME=$test_home SHELL=${RUX_TEST_SHELL:-/bin/bash} \
        PATH="$fixture_root/bin:/usr/bin:/bin" RUX_TEST_ARCHIVE=$fixture_root/release.tar.gz \
        sh "$installer" "$@" >"$output" 2>&1
}

printf '#!/bin/sh\nprintf "%%s\\n" "${RUX_TEST_ARCH:-x86_64}"\n' >"$fixture_root/bin/uname"
printf '%s\n' '#!/bin/sh' \
    'if [ -n "${RUX_TEST_DOWNLOAD_FAIL:-}" ]; then exit 22; fi' \
    'while [ "$#" -gt 0 ]; do' \
    '  if [ "$1" = -o ]; then cp "$RUX_TEST_ARCHIVE" "$2"; exit; fi' \
    '  shift' \
    'done' >"$fixture_root/bin/curl"
printf '%s\n' '#!/bin/sh' \
    'if [ "$1" = -xzf ] && [ -n "${RUX_TEST_EXTRACT_FAIL:-}" ]; then exit 29; fi' \
    'exec /usr/bin/tar "$@"' >"$fixture_root/bin/tar"
chmod +x "$fixture_root/bin/uname" "$fixture_root/bin/curl" "$fixture_root/bin/tar"
printf '#!/bin/sh\nprintf "rux 1.2.3\\n"\n' >"$fixture_root/archive/rux"
chmod +x "$fixture_root/archive/rux"
tar -czf "$fixture_root/release.tar.gz" -C "$fixture_root/archive" rux

# Explicit versions report the selected platform, qualified URL, destination,
# PATH profile, restart action, and timed success summary.
run_installer "$fixture_root/home-explicit" --version v1.2.3 --dir "$fixture_root/install-explicit"
require_output 'Installing Rux v1.2.3 (linux-x86_64)'
require_output "/releases/download/v1.2.3/rux-linux-x86_64.tar.gz'"
require_output "Verifying 'rux-linux-x86_64.tar.gz'"
require_output "Installing to '$fixture_root/install-explicit/rux'"
require_output "Added '$fixture_root/install-explicit' to PATH in '$fixture_root/home-explicit/.bashrc'"
require_output "Restart your terminal to use 'rux' from PATH"
require_output 'Installed Rux in '
require_output "  Binary: '$fixture_root/install-explicit/rux'"
[ -x "$fixture_root/install-explicit/rux" ] || fail 'installer did not install an executable'

# Latest releases and existing PATH entries do not rewrite a shell profile.
mkdir -p "$fixture_root/install-latest" "$fixture_root/home-latest"
HOME=$fixture_root/home-latest SHELL=/bin/bash \
    PATH="$fixture_root/install-latest:$fixture_root/bin:/usr/bin:/bin" \
    RUX_TEST_ARCHIVE=$fixture_root/release.tar.gz \
    sh "$installer" --dir "$fixture_root/install-latest" >"$output" 2>&1
require_output 'Installing latest Rux release (linux-x86_64)'
require_output "PATH already contains '$fixture_root/install-latest'"
[ ! -e "$fixture_root/home-latest/.bashrc" ] || fail 'existing PATH entry rewrote .bashrc'

# AArch64 uses its native release asset, and login-shell selection remains stable.
RUX_TEST_ARCH=aarch64 RUX_TEST_SHELL=/bin/zsh \
    run_installer "$fixture_root/home-arm" --version 1.2.3 --dir "$fixture_root/install-arm"
require_output 'Installing Rux v1.2.3 (linux-aarch64)'
require_output 'rux-linux-aarch64.tar.gz'
require_output "$fixture_root/home-arm/.zshrc'"

set +e
RUX_TEST_ARCH=riscv64 run_installer "$fixture_root/home-bad-arch" --dir "$fixture_root/install-bad" 
status=$?
set -e
[ "$status" -ne 0 ] || fail 'installer accepted an unsupported architecture'
require_output "error: architecture 'riscv64' is not supported by the Linux installer"
require_output "help: build Rux from source: 'https://github.com/rux-lang/Rux'"

set +e
RUX_TEST_DOWNLOAD_FAIL=1 run_installer "$fixture_root/home-download" --version 9.9.9 --dir "$fixture_root/install-download"
status=$?
set -e
[ "$status" -ne 0 ] || fail 'installer accepted a failed download'
require_output "error: failed to download 'https://github.com/rux-lang/Rux/releases/download/v9.9.9/rux-linux-x86_64.tar.gz'"
require_output "help: download the release manually from 'https://github.com/rux-lang/Rux/releases'"

set +e
RUX_TEST_EXTRACT_FAIL=1 run_installer "$fixture_root/home-extract" --dir "$fixture_root/install-extract"
status=$?
set -e
[ "$status" -ne 0 ] || fail 'installer accepted a failed extraction'
require_output "error: failed to extract '"
require_output "help: extract the archive manually and copy 'rux' to '$fixture_root/install-extract'"

powershell_installer=Packaging/Windows/PowerShell/install.ps1
msi_script=Packaging/Windows/Msi/Build.ps1
require_text "$powershell_installer" 'RuntimeInformation]::OSArchitecture'
require_text "$powershell_installer" 'rux-windows-$architecture.zip'
require_text "$powershell_installer" "SetEnvironmentVariable('PATH', \$newPath, 'User')"
require_text "$powershell_installer" "PATH already contains '\$InstallDir'"
require_text "$powershell_installer" "Restart your terminal to use 'rux' from PATH"
require_text Packaging/Windows/Msi/Rux.wxs 'Uninstall it before installing this version.'
require_text "$msi_script" 'Building Rux v$Version (windows-x86_64)'
require_text "$msi_script" 'Built Rux MSI in $elapsed'

if command -v pwsh >/dev/null 2>&1; then
    pwsh -NoProfile -File "$script_directory/Check.ps1"
fi

printf 'Installer message policy tests passed.\n'
