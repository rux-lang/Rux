#!/usr/bin/env sh
# Smoke and parity checks for the PowerShell and POSIX repository entry points.
set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
repository_root=$(CDPATH= cd -P "$script_directory/../../.." && pwd)
fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/rux-script-messages.XXXXXX")
trap 'rm -rf "$fixture_root"' EXIT HUP INT TERM

output=$fixture_root/output.txt
mkdir -p "$fixture_root/bin"
fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}
require_text() {
    file=$1
    expected=$2
    grep -F "$expected" "$file" >/dev/null || fail "'$file' does not contain required script text: $expected"
}
require_output() {
    expected=$1
    if ! grep -F "$expected" "$output" >/dev/null; then
        printf 'error: expected script output containing: %s\n' "$expected" >&2
        sed 's/^/  /' "$output" >&2
        exit 1
    fi
}
write_tool() {
    name=$1
    status=$2
    printf '#!/bin/sh\nexit %s\n' "$status" >"$fixture_root/bin/$name"
    chmod +x "$fixture_root/bin/$name"
}
cd "$repository_root"
# Help is the no-argument behavior, so the entry point is discoverable on its own.
for invocation in '' '--help' 'help'; do
    # shellcheck disable=SC2086
    sh Run.sh $invocation >"$output"
    require_output 'Usage: sh Run.sh <command> [options]'
done
# Both entry points must offer the same commands under the same names.
for expected_command in build test format policy tidy unit clean help; do
    require_output "  $expected_command "
    require_text Run.ps1 "  $expected_command "
done
require_text Run.ps1 '.SYNOPSIS'
require_text Run.ps1 '[CmdletBinding()]'
require_text Run.ps1 'Scripts/RepositoryMessages.ps1'
require_text Run.sh 'Scripts/RepositoryMessages.sh'
if sh Run.sh rux-msg-014-command >"$output" 2>&1; then
    fail 'Run.sh accepted an unknown command'
fi
require_output "error: unknown command 'rux-msg-014-command'"
if sh Run.sh format --clang-tidy >"$output" 2>&1; then
    fail 'Run.sh accepted an option the command does not take'
fi
require_output "error: option '--clang-tidy' is not valid for command 'format'"
duration=$(sh -c '. ./Scripts/RepositoryMessages.sh; format_duration 65')
[ "$duration" = '1 min 5.0 s' ] || fail "POSIX duration formatter returned '$duration'"
if command -v pwsh >/dev/null 2>&1; then
    powershell_helper=$repository_root/Scripts/RepositoryMessages.ps1
    if command -v cygpath >/dev/null 2>&1; then
        powershell_helper=$(cygpath -w "$powershell_helper")
    fi
    duration=$(pwsh -NoProfile -Command \
        ". '$powershell_helper'; Format-Duration -Duration ([TimeSpan]::FromSeconds(65))")
    [ "$duration" = '1 min 5.0 s' ] || fail "PowerShell duration formatter returned '$duration'"
fi
# Prerequisite and child-command failures are checked without configuring a build.
write_tool cmake 0
write_tool ninja 0
if PATH="$fixture_root/bin:$PATH" sh Run.sh build --compiler rux-msg-013-missing >"$output" 2>&1; then
    fail 'Run.sh accepted a missing compiler'
fi
require_output "error: C++ compiler 'rux-msg-013-missing' was not found"
write_tool cmake 23
write_tool clang++ 0
set +e
PATH="$fixture_root/bin:$PATH" sh Run.sh build --compiler "$fixture_root/bin/clang++" >"$output" 2>&1
status=$?
set -e
[ "$status" -eq 23 ] || fail "Run.sh did not preserve child exit code 23 (received $status)"
require_output "error: command 'cmake' failed with exit code 23"
# Check and fix modes use identical counts and outcome vocabulary without touching sources.
write_tool clang-format-22 0
write_tool rux 0
NO_COLOR=1 PATH="$fixture_root/bin:$PATH" sh Run.sh format --check \
    --rux-executable "$fixture_root/bin/rux" >"$output" 2>&1
require_output 'Checking C++ formatting ('
require_output 'Checking Rux formatting ('
require_output 'Finished format check in '
if LC_ALL=C grep "$(printf '\033')" "$output" >/dev/null; then
    fail 'Run.sh emitted ANSI styling when output was redirected with NO_COLOR'
fi
NO_COLOR=1 PATH="$fixture_root/bin:$PATH" sh Run.sh format \
    --rux-executable "$fixture_root/bin/rux" >"$output" 2>&1
require_output 'Formatting C++ sources ('
require_output 'Formatting Rux sources ('
require_output 'Finished source formatting in '
# Labels shared by the POSIX and PowerShell entry points.
for label in \
    'Checking source-tree policy' \
    'Configuring ' \
    'Building compiler and unit tests' \
    'Finished build in ' \
    'Checking C++ formatting (' \
    'Checking Rux formatting (' \
    'Finished format check in ' \
    'Skipping compiler build' \
    'Running clang-tidy (' \
    'Running C++ unit tests' \
    'Removing build outputs' \
    'Finished test workflow in '; do
    require_text Run.sh "$label"
    require_text Run.ps1 "$label"
done
require_text Run.sh 'if [ "$skip_build" = false ]'
require_text Run.ps1 'if (-not $SkipBuild)'
require_text Run.sh 'if [ "$check" = true ]'
require_text Run.ps1 'if ($Check)'
require_text Scripts/RepositoryMessages.sh '[ -t 1 ] && [ -z "${NO_COLOR:-}" ]'

printf 'Repository script message policy tests passed.\n'
