#!/bin/sh

# Build and verify Rux on one POSIX target.
#
# This runs unchanged on a native Linux or macOS runner and inside the FreeBSD
# guest, so the two paths cannot drift. .github/Scripts/Verify.ps1 is the
# Windows peer. Everything it does goes through ./Run.sh, the same entry point
# developers use, so a green job means the developer workflow is green.
#
# Usage: sh .github/Scripts/Verify.sh --stage build|test|format|closure

set -eu

stage=

while [ "$#" -gt 0 ]; do
    case "$1" in
    --stage)
        [ "$#" -ge 2 ] || { printf 'error: option --stage requires a value\n' >&2; exit 1; }
        stage=$2
        shift 2
        ;;
    *)
        printf "error: unknown option '%s'\n" "$1" >&2
        exit 1
        ;;
    esac
done

jobs=$(sh Scripts/TestJobs.sh)
rux=./Bin/rux

case "$stage" in
build)
    # PCH is disabled so ccache sees ordinary translation units; the two defeat
    # each other otherwise.
    #
    # RUX_OSX_ARCHITECTURE builds macos-x86_64 on the AArch64 runner: LLVM
    # publishes no macOS 23.1.0 archive and Homebrew has no Intel bottle for
    # macOS 26, but the macOS SDK is universal, so the AArch64 toolchain emits
    # x86-64 and Rosetta runs the result on the same machine.
    if [ -n "${RUX_OSX_ARCHITECTURE:-}" ]; then
        sh Run.sh build --no-pch --osx-architecture "$RUX_OSX_ARCHITECTURE"
    else
        sh Run.sh build --no-pch
    fi
    ;;
test)
    sh Run.sh unit --jobs "$jobs"
    "$rux" check
    "$rux" lint
    "$rux" test --release --jobs "$jobs"
    ;;
format)
    # Rux formatting needs a built compiler, so it runs here rather than in the
    # quality job. C++ formatting is checked there, without a build.
    find Packages Tests -name Rux.toml -print |
        grep -v 'Tests/Unit/Golden' |
        while IFS= read -r manifest; do
            "$rux" --manifest "$manifest" fmt --check
        done
    ;;
closure)
    # The shipped compiler must depend only on system libraries: a toolchain
    # that is present on the runner is not present on a user's machine.
    case "$(uname -s)" in
    Darwin)
        dependencies=$(otool -L "$rux" | tail -n +2 | awk '{ print $1 }')
        pattern='^(/usr/lib/|/System/Library/|@rpath/$)'
        ;;
    *)
        dependencies=$(ldd "$rux" | awk '{ print $1 }' | grep -v '^linux-vdso' || true)
        pattern='^(/lib|/usr/lib|lib[a-z0-9_+.-]*\.so)'
        ;;
    esac

    unexpected=$(printf '%s\n' "$dependencies" | grep -Ev "$pattern" || true)
    if [ -n "$unexpected" ]; then
        printf 'error: %s links against non-system libraries:\n%s\n' "$rux" "$unexpected" >&2
        exit 1
    fi
    printf '%s depends only on system libraries\n' "$rux"
    ;;
*)
    printf "error: option --stage must be build, test, format, or closure\n" >&2
    exit 1
    ;;
esac
