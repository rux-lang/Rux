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
workflows=$repository_root/.github/workflows
scripts=$repository_root/.github/Scripts
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

# Write a stub tool on the fixture PATH that prints one line.
stub_tool() {
    printf '#!/bin/sh\nprintf "%%s\\n" "%s"\n' "$2" >"$fixture_root/bin/$1"
    chmod +x "$fixture_root/bin/$1"
}

# The manifest is dot-sourced by the setup scripts and packers, so anything
# that is not a comment or a plain assignment is an injection surface, not a
# typo. Every pin it carries is named here so a bump that drops one fails
# before a packer reaches the network.
check_manifest_shape() {
    [ -f "$manifest" ] || fail "'$manifest' was not found"
    if grep -nEv '^[[:space:]]*(#.*)?$|^[A-Z0-9_]+=[A-Za-z0-9._:/+-]*$' "$manifest"; then
        fail "$manifest has a line that is neither a comment nor a plain assignment"
    fi

    for key in TOOLCHAIN_CACHE_EPOCH CCACHE_MAXSIZE LLVM_VERSION CCACHE_VERSION FREEBSD_VERSION \
        LLVM_BASE_URL BREW_LLVM_FORMULA BREW_LLVM_VERSION CCACHE_BASE_URL \
        FREEBSD_PKG_LLVM FREEBSD_BASE_URL FREEBSD_BASE_ASSET SHA256_FREEBSD_BASE_AARCH64; do
        grep -q "^$key=" "$manifest" || fail "$manifest declares no $key"
    done

    for slot in LINUX_X86_64 LINUX_AARCH64 WINDOWS_X86_64 WINDOWS_AARCH64; do
        grep -q "^LLVM_ASSET_$slot=" "$manifest" || fail "$manifest declares no LLVM_ASSET_$slot"
        grep -q "^SHA256_LLVM_$slot=" "$manifest" || fail "$manifest declares no SHA256_LLVM_$slot"
    done

    for slot in LINUX_X86_64 LINUX_AARCH64 MACOS WINDOWS_X86_64 WINDOWS_AARCH64; do
        grep -q "^CCACHE_ASSET_$slot=" "$manifest" || fail "$manifest declares no CCACHE_ASSET_$slot"
        grep -q "^SHA256_CCACHE_$slot=" "$manifest" || fail "$manifest declares no SHA256_CCACHE_$slot"
    done

    # A checksum still recorded as TBD names an input nobody verified; the
    # packers refuse it, so it must never reach them.
    grep '^SHA256_' "$manifest" | while IFS='=' read -r key value; do
        case "$value" in
        *[!0-9a-f]* | '') fail "$key is not 64 lowercase hex digits" ;;
        esac
        [ "${#value}" -eq 64 ] || fail "$key is not 64 hex digits"
    done

    grep '_URL=' "$manifest" | while IFS='=' read -r key value; do
        case "$value" in
        https://*) ;;
        *) fail "$key must be an https URL" ;;
        esac
    done
}

# The manifest is the single source of truth only if it agrees with the build
# system and the documentation it pins.
check_versions_agree() {
    . "$manifest"

    declared=$(head -1 "$repository_root/CMakeLists.txt")
    [ "$declared" = 'cmake_minimum_required(VERSION 3.31...4.4)' ] ||
        fail "CMakeLists.txt must start with cmake_minimum_required(VERSION 3.31...4.4), found: $declared"
    grep -qF '1.13.2' "$repository_root/CMakeLists.txt" ||
        fail 'CMakeLists.txt does not mention the required Ninja version 1.13.2'

    case "$LLVM_VERSION" in
    23.*) ;;
    *) fail "the pinned LLVM version $LLVM_VERSION is outside major 23" ;;
    esac
    [ "$BREW_LLVM_VERSION" = "$LLVM_VERSION" ] ||
        fail "BREW_LLVM_VERSION $BREW_LLVM_VERSION does not match LLVM_VERSION $LLVM_VERSION"
    case "$LLVM_BASE_URL" in
    *"llvmorg-$LLVM_VERSION"*) ;;
    *) fail "LLVM_BASE_URL does not point at the llvmorg-$LLVM_VERSION release" ;;
    esac
    for slot in LINUX_X86_64 LINUX_AARCH64 WINDOWS_X86_64 WINDOWS_AARCH64; do
        eval "asset=\$LLVM_ASSET_$slot"
        case "$asset" in
        *"$LLVM_VERSION"*) ;;
        *) fail "LLVM_ASSET_$slot ($asset) is not an LLVM $LLVM_VERSION asset" ;;
        esac
    done

    case "$CCACHE_BASE_URL" in
    *"v$CCACHE_VERSION"*) ;;
    *) fail "CCACHE_BASE_URL does not point at the ccache v$CCACHE_VERSION release" ;;
    esac
    for slot in LINUX_X86_64 LINUX_AARCH64 MACOS WINDOWS_X86_64 WINDOWS_AARCH64; do
        eval "asset=\$CCACHE_ASSET_$slot"
        case "$asset" in
        *"$CCACHE_VERSION"*) ;;
        *) fail "CCACHE_ASSET_$slot ($asset) is not a ccache $CCACHE_VERSION asset" ;;
        esac
    done

    case "$FREEBSD_BASE_URL" in
    *"$FREEBSD_VERSION-RELEASE"*) ;;
    *) fail "FREEBSD_BASE_URL does not point at the $FREEBSD_VERSION-RELEASE distribution" ;;
    esac

    for document in README.md Docs/Workflow.md AGENTS.md; do
        grep -qF 'CMake 3.31' "$repository_root/$document" ||
            fail "$document does not state the CMake 3.31 requirement"
    done
}

# A manifest carrying anything but assignments must be refused before it is
# sourced, not after. Every reader gets the same hostile file.
check_readers_reject_injection() {
    hostile=$fixture_root/hostile/.github
    mkdir -p "$hostile/Scripts/Toolchain"
    cp "$scripts/SetupToolchain.sh" "$hostile/Scripts/"
    cp "$scripts/Toolchain/PackLinux.sh" "$hostile/Scripts/Toolchain/"
    printf 'CCACHE_MAXSIZE=400M\nLLVM_VERSION=$(touch %s/pwned)\n' \
        "$fixture_root" >"$hostile/Toolchains.env"

    if sh "$hostile/Scripts/SetupToolchain.sh" --target linux-x86_64 \
        --prefix "$fixture_root/hostile-prefix" >"$output" 2>&1; then
        fail 'SetupToolchain.sh accepted a manifest containing a command substitution'
    fi
    [ ! -f "$fixture_root/pwned" ] || fail 'SetupToolchain.sh executed manifest content'

    if sh "$hostile/Scripts/Toolchain/PackLinux.sh" --target linux-x86_64 \
        --prefix "$fixture_root/hostile-prefix" >"$output" 2>&1; then
        fail 'PackLinux.sh accepted a manifest containing a command substitution'
    fi
    [ ! -f "$fixture_root/pwned" ] || fail 'PackLinux.sh executed manifest content'

    if sh "$scripts/SetupToolchain.sh" --target freebsd-x86_64 >"$output" 2>&1; then
        fail 'SetupToolchain.sh accepted a target that has no host prefix'
    fi
    require_output "unsupported target 'freebsd-x86_64'"
}

# The setup script packs when the prefix does not match the manifest and the
# packer, skips the packer when it does, and exports what every later step
# needs. A stub packer makes that observable without the network, and stub
# cmake and ninja stand in for the runner image.
check_setup_packs_on_miss() {
    installed=$fixture_root/installed/.github
    mkdir -p "$installed/Scripts/Toolchain"
    cp "$scripts/SetupToolchain.sh" "$installed/Scripts/"
    printf 'TOOLCHAIN_CACHE_EPOCH=1\nCCACHE_MAXSIZE=400M\n' >"$installed/Toolchains.env"

    cat >"$installed/Scripts/Toolchain/PackLinux.sh" <<'STUB'
#!/bin/sh
printf '%s\n' "$*" >>"$RUX_STUB_LOG"
prefix=
while [ "$#" -gt 0 ]; do
    case "$1" in
    --prefix) prefix=$2; shift 2 ;;
    *) shift ;;
    esac
done
mkdir -p "$prefix/bin"
for tool in clang++-23 clang-format-23 clang-tidy-23 ccache; do
    printf '#!/bin/sh\nprintf "stub %s\\n"\n' "$tool" >"$prefix/bin/$tool"
    chmod +x "$prefix/bin/$tool"
done
STUB

    stub_tool cmake 'cmake version 3.31.0'
    stub_tool ninja '1.13.2'

    prefix=$fixture_root/prefix-installed
    packed=$fixture_root/packed.txt
    : >"$packed"
    environment=$fixture_root/github-env
    path_file=$fixture_root/github-path

    run_setup() {
        : >"$environment"
        : >"$path_file"
        RUX_STUB_LOG=$packed GITHUB_ENV=$environment GITHUB_PATH=$path_file \
            PATH="$fixture_root/bin:$PATH" \
            sh "$installed/Scripts/SetupToolchain.sh" --target linux-x86_64 --prefix "$prefix" \
            >"$output" 2>&1
    }

    run_setup || {
        sed 's/^/  /' "$output" >&2
        fail 'SetupToolchain.sh failed on a cold prefix'
    }
    [ "$(wc -l <"$packed" | tr -d '[:space:]')" -eq 1 ] || fail 'SetupToolchain.sh did not pack a cold prefix once'
    grep -q -- "--target linux-x86_64" "$packed" || fail 'the packer was not told the target'
    grep -qF -- "--prefix $prefix" "$packed" || fail 'the packer was not told the prefix'

    for key in RUX_TOOLCHAIN CXX CMAKE_CXX_COMPILER_LAUNCHER CCACHE_DIR \
        CCACHE_MAXSIZE CCACHE_COMPILERCHECK CCACHE_NOHASHDIR; do
        grep -q "^$key=" "$environment" ||
            fail "SetupToolchain.sh did not export $key"
    done
    grep -qxF "$prefix/bin" "$path_file" ||
        fail 'SetupToolchain.sh did not put the prefix on PATH'

    run_setup || fail 'SetupToolchain.sh failed on a warm prefix'
    [ "$(wc -l <"$packed" | tr -d '[:space:]')" -eq 1 ] || fail 'SetupToolchain.sh repacked a prefix that matched'
    require_output 'already present'

    printf 'TOOLCHAIN_CACHE_EPOCH=2\nCCACHE_MAXSIZE=400M\n' >"$installed/Toolchains.env"
    run_setup || fail 'SetupToolchain.sh failed after the manifest changed'
    [ "$(wc -l <"$packed" | tr -d '[:space:]')" -eq 2 ] || fail 'SetupToolchain.sh kept a prefix whose manifest changed'

    # The runner image's CMake is checked against the floor CMakeLists.txt
    # declares, so an image that regresses is named rather than guessed at.
    stub_tool cmake 'cmake version 3.30.5'
    if run_setup; then
        fail 'SetupToolchain.sh accepted a CMake older than 3.31'
    fi
    require_output '3.31'
}

# A checksum still recorded as TBD must stop the packer before it fetches
# anything: a stub curl records whether it was ever reached.
check_packer_refuses_tbd() {
    unpublished=$fixture_root/unpublished/.github
    mkdir -p "$unpublished/Scripts/Toolchain"
    cp "$scripts/Toolchain/PackLinux.sh" "$unpublished/Scripts/Toolchain/"
    {
        printf 'LLVM_VERSION=23.1.0\n'
        printf 'CCACHE_VERSION=4.14\n'
        printf 'LLVM_BASE_URL=https://example.invalid/llvm\n'
        printf 'LLVM_ASSET_LINUX_X86_64=LLVM-23.1.0-Linux-X64.tar.zst\n'
        printf 'SHA256_LLVM_LINUX_X86_64=TBD\n'
        printf 'CCACHE_BASE_URL=https://example.invalid/ccache\n'
        printf 'CCACHE_ASSET_LINUX_X86_64=ccache-4.14-linux-x86_64-musl-static.tar.xz\n'
        printf 'SHA256_CCACHE_LINUX_X86_64=%s\n' \
            0000000000000000000000000000000000000000000000000000000000000000
    } >"$unpublished/Toolchains.env"

    printf '#!/bin/sh\ntouch "%s/downloaded"\nexit 1\n' "$fixture_root" >"$fixture_root/bin/curl"
    chmod +x "$fixture_root/bin/curl"

    if PATH="$fixture_root/bin:$PATH" sh "$unpublished/Scripts/Toolchain/PackLinux.sh" \
        --target linux-x86_64 --prefix "$fixture_root/prefix-unpublished" >"$output" 2>&1; then
        fail 'PackLinux.sh packed a toolchain whose checksum is still TBD'
    fi
    require_output TBD
    [ ! -f "$fixture_root/downloaded" ] || fail 'PackLinux.sh reached for the network with a TBD checksum'
    rm -f "$fixture_root/bin/curl"
}

# The docs-only classifier decides whether a run builds anything, so the
# boundary is pinned down here: documentation and community metadata are
# docs, everything else — including Markdown a test might read — is code, and
# an empty list is code.
check_scope_classifier() {
    scope=$scripts/Scope.sh

    classify() {
        printf '%s' "$1" | sh "$scope" classify
    }

    [ "$(classify 'Docs/CI-CD.md
README.md
')" = docs ] || fail 'a documentation change was not classified as docs'
    [ "$(classify 'Docs/Platforms/FreeBSD.md
')" = docs ] || fail 'a nested documentation change was not classified as docs'
    [ "$(classify '.github/ISSUE_TEMPLATE/BugReport.yml
.github/FUNDING.yml
.github/SECURITY.md
LICENSE.md
')" = docs ] || fail 'community metadata was not classified as docs'
    [ "$(classify 'Docs/CI-CD.md
Compiler/Main.cpp
')" = code ] || fail 'a code change beside documentation was classified as docs'
    [ "$(classify 'Tests/README.md
')" = code ] || fail 'Markdown outside Docs/ was classified as docs'
    [ "$(classify 'Packages/Core/LICENSE.md
')" = code ] || fail 'a package license was classified as docs'
    [ "$(classify '.github/workflows/CI.yml
')" = code ] || fail 'a workflow change was classified as docs'
    [ "$(classify '')" = code ] || fail 'an empty change list was classified as docs'

    tier() {
        env -i PATH="$PATH" "$@" sh "$scope" tier
    }

    [ "$(tier GITHUB_EVENT_NAME=push GITHUB_REF_NAME=feat/x)" = fast ] ||
        fail 'a topic-branch push is not the fast lane'
    [ "$(tier GITHUB_EVENT_NAME=push GITHUB_REF_NAME=dev)" = extended ] ||
        fail 'a push to dev is not extended'
    [ "$(tier GITHUB_EVENT_NAME=push GITHUB_REF_NAME=main)" = extended ] ||
        fail 'a push to main is not extended'
    [ "$(tier GITHUB_EVENT_NAME=pull_request GITHUB_REF_NAME=7/merge)" = full ] ||
        fail 'a pull request is not full'
    [ "$(tier GITHUB_EVENT_NAME=workflow_dispatch GITHUB_REF_NAME=dev)" = extended ] ||
        fail 'a dispatched run is not extended by default'
    [ "$(tier GITHUB_EVENT_NAME=workflow_dispatch GITHUB_REF_NAME=dev RUX_REQUESTED_SCOPE=fast)" = fast ] ||
        fail 'a requested scope did not win'
    if tier GITHUB_EVENT_NAME=workflow_dispatch RUX_REQUESTED_SCOPE=nightly >"$output" 2>&1; then
        fail 'Scope.sh accepted an unknown requested scope'
    fi
}

# Each target has a reusable workflow of its own carrying every step the
# target runs. CI.yml calls all eight with the scope of the run and Release.yml
# calls all eight with the extended scope, so a release is built from the very
# same steps; each uploads the compiler under the name Release.yml downloads.
# A target workflow has no triggers of its own: CI.yml decides.
check_target_workflows() {
    ci=$workflows/CI.yml
    release=$workflows/Release.yml
    [ -f "$ci" ] || fail 'CI.yml was not found'
    [ -f "$release" ] || fail 'Release.yml was not found'
    for pair in linux-x86_64:Linux-x86_64 linux-aarch64:Linux-AArch64 \
        macos-aarch64:macOS-AArch64 macos-x86_64:macOS-x86_64 \
        windows-x86_64:Windows-x86_64 windows-aarch64:Windows-AArch64 \
        freebsd-x86_64:FreeBSD-x86_64 freebsd-aarch64:FreeBSD-AArch64; do
        target=${pair%%:*}
        file=$workflows/${pair#*:}.yml
        name=$(basename "$file")
        [ -f "$file" ] || fail "target '$target' has no workflow at $file"
        grep -q '^  workflow_call:' "$file" || fail "$name cannot be called by CI.yml"
        grep -q '^      scope:' "$file" || fail "$name takes no scope input"
        if grep -qE '^  (push|pull_request|schedule|workflow_dispatch):' "$file"; then
            fail "$name triggers on its own; CI.yml decides when a target runs"
        fi
        if grep -qE 'matrix\.|inputs\.targets' "$file"; then
            fail "$name conditions its steps on a matrix"
        fi
        grep -q "name: rux-$target\$" "$file" ||
            fail "$name does not upload the compiler as rux-$target"
        grep -q "uses: ./.github/workflows/$name\$" "$ci" || fail "CI.yml does not call $name"
        grep -q "uses: ./.github/workflows/$name\$" "$release" || fail "Release.yml does not call $name"
    done
    [ "$(grep -c '^      scope: extended$' "$release")" -eq 8 ] ||
        fail 'Release.yml does not call every target with the extended scope'
    if grep -qE '^  schedule:' "$ci"; then
        fail 'CI.yml runs on a schedule; verification is driven by pushes, pull requests, and dispatch'
    fi
}

# What the redesign retired must stay retired: the shared build workflow, the
# separate quality workflow, the hand-rolled guest driver, and every mention
# of the external toolchain repository outside the changelog's history.
check_retired_paths() {
    for retired in workflows/Build.yml workflows/CodeQuality.yml Scripts/FreeBSDVM.sh; do
        [ ! -e "$repository_root/.github/$retired" ] ||
            fail ".github/$retired is back; its role moved into CI.yml and the vmactions guests"
    done
    mentions=$(cd "$repository_root" && git grep -l -E 'rux-lang/Toolchain|FreeBSDVM\.sh|TOOLCHAIN_REVISION' -- . \
        ':!CHANGELOG.md' ':!Tests/Scripts/CI/Check.sh' 2>/dev/null || true)
    [ -z "$mentions" ] || fail "retired CI names are still referenced by: $(printf '%s' "$mentions" | tr '\n' ' ')"
}

# Both FreeBSD workflows boot the same prepared guest, which vmactions caches
# keyed on the prepare script; a byte of difference costs a second multi-GB
# disk in the cache and a second package installation on every miss.
extract_prepare() {
    awk '
        /^[[:space:]]*prepare: \|[[:space:]]*$/ { indent = match($0, /[^ ]/) - 1; active = 1; next }
        active {
            if ($0 ~ /^[[:space:]]*$/) { print; next }
            if (match($0, /[^ ]/) - 1 <= indent) { active = 0; next }
            print
        }' "$1" | tr -d '\r'
}

check_freebsd_prepare_scripts_match() {
    extract_prepare "$workflows/FreeBSD-x86_64.yml" >"$fixture_root/prepare-x86_64"
    extract_prepare "$workflows/FreeBSD-AArch64.yml" >"$fixture_root/prepare-aarch64"
    [ -s "$fixture_root/prepare-x86_64" ] || fail 'FreeBSD-x86_64.yml has no prepare script'
    [ -s "$fixture_root/prepare-aarch64" ] || fail 'FreeBSD-AArch64.yml has no prepare script'
    cmp -s "$fixture_root/prepare-x86_64" "$fixture_root/prepare-aarch64" ||
        fail 'the FreeBSD prepare scripts differ, so the guests cannot share one cached disk'
}

# A third-party action is pinned to a commit, not a tag that can move, and a
# checkout in a workflow that never pushes keeps no credentials.
check_actions_are_pinned() {
    grep -rhoE 'uses: [^ ]+' "$workflows" "$repository_root/.github/actions" |
        sed 's/^uses: //' | sort -u >"$fixture_root/uses"
    [ -s "$fixture_root/uses" ] || fail 'no actions are used by the workflows'
    while IFS= read -r use; do
        case "$use" in
        ./* | actions/*) continue ;;
        esac
        reference=${use##*@}
        case "$reference" in
        *[!0-9a-f]* | '') fail "$use is not pinned to a commit" ;;
        esac
        [ "${#reference}" -eq 40 ] || fail "$use is not pinned to a full commit hash"
    done <"$fixture_root/uses"

    for file in "$workflows"/CI.yml "$workflows"/*-x86_64.yml "$workflows"/*-AArch64.yml; do
        checkouts=$(grep -c 'actions/checkout@' "$file" || true)
        guarded=$(grep -c 'persist-credentials: false' "$file" || true)
        [ "$checkouts" -eq "$guarded" ] ||
            fail "$(basename "$file") checks out with credentials it never uses"
    done
}

# Workflows run on a case-sensitive filesystem while this repository is often
# edited on one that is not, so a reference whose case does not match the file
# resolves locally and fails only once GitHub parses it. Compare against the
# names git recorded rather than asking the filesystem, which would answer with
# its own case rules. A reference may name a directory, such as the composite
# action or the packers' folder.
check_workflow_paths_match_case() {
    tracked=$fixture_root/tracked.txt
    (cd "$repository_root" && git ls-files) >"$tracked" 2>/dev/null ||
        fail 'could not list tracked files'

    referenced=$fixture_root/referenced.txt
    grep -rhoE '(\./)?(\.github/(workflows|Scripts|actions)/[A-Za-z0-9._/-]+|Compiler/CMake/Toolchains/[A-Za-z0-9._-]+)' \
        "$workflows" "$repository_root/.github/actions" |
        sed 's|^\./||; s|/$||' | sort -u >"$referenced"

    [ -s "$referenced" ] || fail 'no .github paths were found in the workflows'

    while IFS= read -r path; do
        grep -qxF "$path" "$tracked" || grep -q "^$path/" "$tracked" ||
            fail "the workflows reference '$path', which no tracked file matches exactly"
    done <"$referenced"
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
check_readers_reject_injection
check_setup_packs_on_miss
check_packer_refuses_tbd
check_scope_classifier
check_target_workflows
check_retired_paths
check_freebsd_prepare_scripts_match
check_actions_are_pinned
check_workflow_paths_match_case
check_tidy_shards_are_disjoint

printf 'CI helper checks passed\n'
