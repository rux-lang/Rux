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
        LLVM_BASE_URL CCACHE_BASE_URL \
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
    # macOS and FreeBSD take their Clang from a rolling package source, which
    # can be held to a major but not to a patch. A patch pin there fails the
    # first cold pack after every LLVM patch release, so none may come back.
    if grep -qE '^(BREW|FREEBSD_PKG)_[A-Z_]*VERSION=' "$manifest"; then
        fail "$manifest pins a patch version for a rolling package source"
    fi
    grep -qF 'formula=llvm@$llvm_major' "$scripts/Toolchain/PackMacOS.sh" ||
        fail 'PackMacOS.sh does not take the Homebrew formula from the pinned LLVM major'
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

    # AGENTS.md is local contributor configuration, absent from a clean checkout.
    for document in README.md Docs/Workflow.md; do
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

# Exercise the packer's actual selection and extraction with a tiny archive
# containing directory entries and their children, as the LLVM releases do.
check_linux_archive_extraction() {
    archive_fixture=$fixture_root/archive
    mkdir -p "$archive_fixture/source/LLVM/bin" "$archive_fixture/source/LLVM/lib/clang/23/include/nested" \
        "$archive_fixture/source/LLVM/lib/clang/23/lib/target" "$archive_fixture/llvm"
    for tool in clang clang-format clang-tidy llvm-size clang-scan-deps; do
        printf '%s\n' "$tool" >"$archive_fixture/source/LLVM/bin/$tool"
    done
    ln -s clang "$archive_fixture/source/LLVM/bin/clang++"
    printf 'header\n' >"$archive_fixture/source/LLVM/lib/clang/23/include/nested/header.h"
    printf 'builtins\n' >"$archive_fixture/source/LLVM/lib/clang/23/lib/target/libclang_rt.builtins.a"
    tar -cf "$archive_fixture/llvm.tar" -C "$archive_fixture/source" LLVM
    # Compression is irrelevant to member matching; feed the plain fixture tar.
    # tar stops reading at the end-of-archive marker and may close the pipe
    # before the stub has written the record padding behind it; a real
    # decompressor is drained by the record-sized reads a real archive fills,
    # but the stub must shrug off the resulting SIGPIPE or tar reports it.
    printf '#!/bin/sh\ntrap "" PIPE\ncat 2>/dev/null\nexit 0\n' >"$fixture_root/bin/unzstd"
    chmod +x "$fixture_root/bin/unzstd"
    sed -n '/^tar --use-compress-program="unzstd[^"]*" -tf /,/^rm -f "\$llvm_archive"/p' \
        "$scripts/Toolchain/PackLinux.sh" >"$archive_fixture/extract.sh"
    [ -s "$archive_fixture/extract.sh" ] || fail 'no Linux extraction commands found'
    (
        set -eu
        work=$archive_fixture
        llvm_archive=$archive_fixture/llvm.tar
        llvm_asset=fixture.tar
        llvm_major=23
        PATH="$fixture_root/bin:$PATH"
        export PATH work llvm_archive llvm_asset llvm_major
        # A separate shell preserves errexit even though this subshell is
        # checked with ||; otherwise a later rm could hide tar's failure.
        sh -eu "$archive_fixture/extract.sh"
    ) >"$output" 2>&1 || { cat "$output" >&2; fail 'Linux archive extraction failed'; }
    for member in bin/clang bin/clang++ bin/clang-format bin/clang-tidy bin/llvm-size \
        lib/clang/23/include/nested/header.h lib/clang/23/lib/target/libclang_rt.builtins.a; do
        cmp "$archive_fixture/source/LLVM/$member" "$archive_fixture/llvm/$member" ||
            fail "Linux extraction lost $member"
    done
    [ ! -e "$archive_fixture/llvm/bin/clang-scan-deps" ] || fail 'Linux extraction selected unrelated tools'
    rm -f "$fixture_root/bin/unzstd"
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
    [ "$(tier GITHUB_EVENT_NAME=push GITHUB_REF_NAME=dev)" = full ] ||
        fail 'a push to dev is not full'
    [ "$(tier GITHUB_EVENT_NAME=push GITHUB_REF_NAME=main)" = full ] ||
        fail 'a push to main is not full'
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

# Print one job of a workflow: its key line through the line before the next
# job's key.
job_block() {
    awk -v job="  $2:" '
        $0 == job { active = 1; print; next }
        active && /^  [A-Za-z0-9_-]+:$/ { exit }
        active { print }' "$1"
}

# One workflow verifies every target. CI.yml has a job per target that names
# it in RUX_TARGET, which the shared upload step publishes as rux-<target>,
# the name Release.yml packages; Release.yml builds nothing of its own and
# calls CI.yml in the extended scope, so a release is built by the very jobs
# CI ran. The gate must need every job, or a new job could fail unnoticed.
check_ci_workflow() {
    ci=$workflows/CI.yml
    release=$workflows/Release.yml
    [ -f "$ci" ] || fail 'CI.yml was not found'
    [ -f "$release" ] || fail 'Release.yml was not found'
    others=$(ls "$workflows" | grep -vxE 'CI\.yml|Release\.yml' || true)
    [ -z "$others" ] ||
        fail "verification lives in CI.yml and releases in Release.yml, but there is also: $(printf '%s' "$others" | tr '\n' ' ')"

    grep -q '^  workflow_call:$' "$ci" || fail 'Release.yml cannot call CI.yml'
    if grep -qE '^  schedule:' "$ci"; then
        fail 'CI.yml runs on a schedule; verification is driven by pushes, pull requests, and dispatch'
    fi
    # A called workflow sees its caller's name, which is what keeps a push
    # from cancelling a release that is running these jobs.
    grep -q '^  group: \${{ github.workflow }}-' "$ci" ||
        fail 'the CI.yml concurrency group does not start with the workflow name'

    grep -q '^          name: rux-\${{ env.RUX_TARGET }}$' "$ci" ||
        fail 'the shared upload step does not publish the compiler as rux-<target>'
    for target in linux-x86_64 linux-aarch64 macos-aarch64 macos-x86_64 \
        windows-x86_64 windows-aarch64 freebsd-x86_64 freebsd-aarch64; do
        job_block "$ci" "$target" >"$fixture_root/job"
        [ -s "$fixture_root/job" ] || fail "CI.yml has no job for target '$target'"
        grep -q "^      RUX_TARGET: $target\$" "$fixture_root/job" ||
            fail "the $target job does not name its target in RUX_TARGET"
        grep -qE '^      - [&*]upload$' "$fixture_root/job" ||
            fail "the $target job does not upload its compiler"
    done

    sed -n '/^jobs:$/,$p' "$ci" | grep -E '^  [A-Za-z0-9_-]+:$' | tr -d ' :' >"$fixture_root/jobs"
    job_block "$ci" ci >"$fixture_root/gate"
    while IFS= read -r job; do
        job_block "$ci" "$job" | grep -q '^    timeout-minutes: ' ||
            fail "the $job job has no timeout"
        [ "$job" = ci ] || grep -q "^      - $job\$" "$fixture_root/gate" ||
            fail "the CI gate does not need the $job job, so its failure would pass unnoticed"
    done <"$fixture_root/jobs"

    [ "$(grep -c 'uses: \./\.github/workflows/' "$release")" -eq 1 ] ||
        fail 'Release.yml must build through exactly one workflow call'
    grep -q '^    uses: \./\.github/workflows/CI\.yml$' "$release" || fail 'Release.yml does not call CI.yml'
    grep -q '^      scope: extended$' "$release" || fail 'Release.yml does not call CI.yml with the extended scope'
}

# What the redesigns retired must stay retired: the shared build workflow, the
# separate quality workflow, the hand-rolled guest driver, the per-target and
# cache-filling workflows that CI.yml absorbed, and every mention of them or of
# the external toolchain repository outside the changelog's history.
check_retired_paths() {
    for retired in workflows/Build.yml workflows/CodeQuality.yml Scripts/FreeBSDVM.sh \
        workflows/Toolchain.yml workflows/FreeBSD-Image.yml \
        workflows/Linux-x86_64.yml workflows/Linux-AArch64.yml \
        workflows/macOS-x86_64.yml workflows/macOS-AArch64.yml \
        workflows/Windows-x86_64.yml workflows/Windows-AArch64.yml \
        workflows/FreeBSD-x86_64.yml workflows/FreeBSD-AArch64.yml; do
        [ ! -e "$repository_root/.github/$retired" ] ||
            fail ".github/$retired is back; its role moved into CI.yml"
    done
    mentions=$(cd "$repository_root" && git grep -l -E \
        'rux-lang/Toolchain|FreeBSDVM\.sh|TOOLCHAIN_REVISION|(Linux|macOS|Windows|FreeBSD)-(x86_64|AArch64)\.yml|FreeBSD-Image\.yml|Toolchain\.yml' \
        -- . ':!CHANGELOG.md' ':!Tests/Scripts/CI/Check.sh' 2>/dev/null || true)
    [ -z "$mentions" ] || fail "retired CI names are still referenced by: $(printf '%s' "$mentions" | tr '\n' ' ')"
}

# Both FreeBSD builds boot the same prepared guest, which vmactions caches
# keyed on the prepare script and the sync method; a byte of difference costs
# a second multi-GB disk in the cache and a second package installation on
# every miss. So the script is written once, in the job that prepares the
# disk, and the builds alias it and share one boot step.
extract_prepare() {
    awk '
        /^[[:space:]]*prepare: (&[A-Za-z0-9_-]+ )?\|[[:space:]]*$/ { indent = match($0, /[^ ]/) - 1; active = 1; next }
        active {
            if ($0 ~ /^[[:space:]]*$/) { print; next }
            if (match($0, /[^ ]/) - 1 <= indent) { active = 0; next }
            print
        }' "$1" | tr -d '\r'
}

check_freebsd_guests_share_one_disk() {
    ci=$workflows/CI.yml
    job_block "$ci" freebsd-image >"$fixture_root/image"
    grep -q '^          prepare: &freebsd-prepare |$' "$fixture_root/image" ||
        fail 'the FreeBSD image job does not define the prepare script'
    grep -q '^          copyback: false$' "$fixture_root/image" ||
        fail 'the FreeBSD image job copies its workspace back, so a cold cache waits on the disk upload'
    [ "$(grep -c '^          prepare: ' "$ci")" -eq 2 ] && grep -q '^          prepare: \*freebsd-prepare$' "$ci" ||
        fail 'CI.yml carries a second prepare script, so the guests cannot share one cached disk'
    for job in freebsd-x86_64 freebsd-aarch64; do
        job_block "$ci" "$job" >"$fixture_root/job"
        grep -qE '^      - [&*]freebsd-guest$' "$fixture_root/job" ||
            fail "the $job job does not boot the shared guest step"
        grep -qE '^    needs: \[.*freebsd-image\]$' "$fixture_root/job" ||
            fail "the $job job starts before the FreeBSD image is prepared"
    done
    [ "$(grep -c 'uses: vmactions/freebsd-vm@' "$ci")" -eq "$(grep -c '^          sync: rsync$' "$ci")" ] ||
        fail 'a FreeBSD guest does not sync with rsync, so its disk has a key of its own'

    # Run the actual checksum guard with matching and mismatching digests.
    # sh -n alone cannot catch a missing closing bracket in a test command.
    extract_prepare "$ci" >"$fixture_root/prepare"
    [ -s "$fixture_root/prepare" ] || fail 'CI.yml has no FreeBSD prepare script'
    sed -n '/^[[:space:]]*\[ "$(sha256 /p' "$fixture_root/prepare" |
        sed 's/${{ env.SHA256_FREEBSD_BASE_AARCH64 }}/expected/' >"$fixture_root/checksum.sh"
    [ -s "$fixture_root/checksum.sh" ] || fail 'the FreeBSD prepare script has no checksum guard'
    stub_tool sha256 expected
    PATH="$fixture_root/bin:$PATH" sh "$fixture_root/checksum.sh" || fail 'FreeBSD rejected a matching checksum'
    stub_tool sha256 wrong
    if PATH="$fixture_root/bin:$PATH" sh "$fixture_root/checksum.sh"; then
        fail 'FreeBSD accepted a mismatching checksum'
    fi
    rm -f "$fixture_root/bin/sha256"
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

    for file in "$workflows"/CI.yml; do
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
    (cd "$repository_root" && git ls-files) >"$tracked" 2>"$fixture_root/git-error" ||
        fail "could not list tracked files: $(cat "$fixture_root/git-error")"

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
check_linux_archive_extraction
check_scope_classifier
check_ci_workflow
check_retired_paths
check_freebsd_guests_share_one_disk
check_actions_are_pinned
check_workflow_paths_match_case
check_tidy_shards_are_disjoint

case "$(uname -s)" in
MINGW* | MSYS*) pwsh -NoProfile -File "$script_directory/CheckWindows.ps1" ;;
esac

printf 'CI helper checks passed\n'
