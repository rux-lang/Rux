#!/usr/bin/env sh
# Checks for the CI helpers under .github.
#
# Registered as the CTest test Scripts.CI. It runs on every host: the checks are
# plain POSIX shell with no interpreter to find, so the coverage cannot silently
# disappear the way it did when this was a Python suite gated on an optional
# find_package.
#
# Every run owns a private fixture directory and touches no repository file.
set -eu

script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
repository_root=$(CDPATH= cd -P "$script_directory/../../.." && pwd)
fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/rux-ci-check.XXXXXX")
trap 'rm -rf "$fixture_root"' EXIT HUP INT TERM

manifest=$repository_root/.github/Toolchains.env
output=$fixture_root/output.txt
mkdir -p "$fixture_root/bin"

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_output() {
    if ! grep -F -- "$1" "$output" >/dev/null; then
        printf 'error: expected output containing: %s\n' "$1" >&2
        sed 's/^/  /' "$output" >&2
        exit 1
    fi
}

# The manifest is dot-sourced by the setup scripts, so anything that is not a
# comment or a plain assignment is an injection surface, not a typo.
check_manifest_shape() {
    [ -f "$manifest" ] || fail "'$manifest' was not found"
    if grep -nEv '^[[:space:]]*(#.*)?$|^[A-Z0-9_]+=[A-Za-z0-9._:/+-]*$' "$manifest"; then
        fail "$manifest has a line that is neither a comment nor a plain assignment"
    fi

    for key in TOOLCHAIN_REVISION TOOLCHAIN_BASE_URL LLVM_VERSION CMAKE_VERSION \
        NINJA_VERSION FREEBSD_VERSION CCACHE_MAXSIZE TEST_JOBS_MAX; do
        grep -q "^$key=" "$manifest" || fail "$manifest declares no $key"
    done

    for target in LINUX_X86_64 LINUX_AARCH64 MACOS_AARCH64 WINDOWS_X86_64 WINDOWS_AARCH64; do
        grep -q "^SHA256_TOOLCHAIN_$target=" "$manifest" ||
            fail "$manifest declares no SHA256_TOOLCHAIN_$target"
    done

    for image in BUILD_X86_64 BUILD_AARCH64 RUNTIME_X86_64 RUNTIME_AARCH64 MINIMAL_AARCH64; do
        grep -q "^SHA256_IMAGE_$image=" "$manifest" || fail "$manifest declares no SHA256_IMAGE_$image"
    done

    grep '^SHA256_' "$manifest" | while IFS='=' read -r key value; do
        case "$value" in
        TBD) ;;
        *)
            case "$value" in
            *[!0-9a-f]* | '') fail "$key is neither TBD nor 64 lowercase hex digits" ;;
            esac
            [ "${#value}" -eq 64 ] || fail "$key is not 64 hex digits"
            ;;
        esac
    done

    base=$(sed -n 's/^TOOLCHAIN_BASE_URL=//p' "$manifest")
    case "$base" in
    https://*) ;;
    *) fail "TOOLCHAIN_BASE_URL must be an https URL" ;;
    esac
}

# The manifest is the single source of truth only if it agrees with the build
# system it pins.
check_versions_agree() {
    . "$manifest"

    declared=$(sed -n 's/^cmake_minimum_required(VERSION \([0-9.]*\)).*/\1/p' \
        "$repository_root/CMakeLists.txt" | head -1)
    [ "$declared" = "$CMAKE_VERSION" ] ||
        fail "CMakeLists.txt requires CMake $declared but the manifest pins $CMAKE_VERSION"

    grep -qF "$NINJA_VERSION" "$repository_root/CMakeLists.txt" ||
        fail "CMakeLists.txt does not mention the pinned Ninja version $NINJA_VERSION"

    case "$LLVM_VERSION" in
    23.*) ;;
    *) fail "the pinned LLVM version $LLVM_VERSION is outside major 23" ;;
    esac
}

# A bundle whose bytes do not match the manifest must never be installed, and a
# revision that has not been published yet must fail loudly rather than fetch.
check_setup_rejects_unpublished() {
    if sh "$repository_root/.github/Scripts/SetupToolchain.sh" \
        --target linux-x86_64 --prefix "$fixture_root/prefix" >"$output" 2>&1; then
        fail 'SetupToolchain.sh installed a toolchain from an unpublished manifest'
    fi
    require_output TBD
    [ ! -d "$fixture_root/prefix" ] ||
        fail 'SetupToolchain.sh created a prefix for an unpublished manifest'

    if sh "$repository_root/.github/Scripts/SetupToolchain.sh" \
        --target freebsd-x86_64 >"$output" 2>&1; then
        fail 'SetupToolchain.sh accepted a target that has no host bundle'
    fi
    require_output "unsupported target 'freebsd-x86_64'"
}

# A manifest carrying anything but assignments must be refused before it is
# sourced, not after.
check_setup_rejects_injection() {
    hostile=$fixture_root/hostile/.github
    mkdir -p "$hostile/Scripts"
    cp "$repository_root/.github/Scripts/SetupToolchain.sh" "$hostile/Scripts/"
    printf 'TOOLCHAIN_REVISION=1\nSHA256_TOOLCHAIN_LINUX_X86_64=$(touch %s/pwned)\n' \
        "$fixture_root" >"$hostile/Toolchains.env"

    if sh "$hostile/Scripts/SetupToolchain.sh" --target linux-x86_64 >"$output" 2>&1; then
        fail 'SetupToolchain.sh accepted a manifest containing a command substitution'
    fi
    [ ! -f "$fixture_root/pwned" ] || fail 'SetupToolchain.sh executed manifest content'
}

# Running every shard must cover each translation unit exactly once. A stub
# clang-tidy makes that observable without a real analysis run.
check_tidy_shards_are_disjoint() {
    build=$fixture_root/Build
    mkdir -p "$build"
    {
        printf '[\n'
        index=0
        while [ "$index" -lt 17 ]; do
            [ "$index" -eq 0 ] || printf ',\n'
            printf '  { "directory": "%s", "command": "clang++", "file": "%s/Compiler/Unit%02d.cpp" }' \
                "$build" "$repository_root" "$index"
            index=$((index + 1))
        done
        printf '\n]\n'
    } >"$build/compile_commands.json"

    printf '#!/bin/sh\nprintf "%%s\\n" "$*" >> "%s/analyzed.txt"\n' "$fixture_root" \
        >"$fixture_root/bin/clang-tidy-23"
    chmod +x "$fixture_root/bin/clang-tidy-23"
    : >"$fixture_root/analyzed.txt"

    for shard in 0 1 2; do
        (
            cd "$repository_root" &&
                PATH="$fixture_root/bin:$PATH" sh Run.sh tidy \
                    --build-directory "$build" --shard-index "$shard" --shard-count 3
        ) >"$output" 2>&1 || fail "shard $shard failed"
    done

    analyzed=$(grep -c 'Unit[0-9][0-9]\.cpp' "$fixture_root/analyzed.txt" || true)
    [ "$analyzed" -eq 17 ] || fail "the three shards analyzed $analyzed files, not 17"

    unique=$(grep -o 'Unit[0-9][0-9]\.cpp' "$fixture_root/analyzed.txt" | sort -u | wc -l)
    [ "$unique" -eq 17 ] || fail "the three shards overlapped: $unique distinct files of 17"

    if (cd "$repository_root" && sh Run.sh tidy --build-directory "$build" \
        --shard-index 3 --shard-count 3) >"$output" 2>&1; then
        fail 'Run.sh accepted a shard index equal to the shard count'
    fi
    require_output 'must be less than'
}

check_manifest_shape
check_versions_agree
check_setup_rejects_unpublished
check_setup_rejects_injection
check_tidy_shards_are_disjoint

printf 'CI helper checks passed\n'
