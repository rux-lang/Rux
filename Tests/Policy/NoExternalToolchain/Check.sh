#!/usr/bin/env sh
# External-toolchain guard.
#
# Rux produces its own machine code, its own object files and its own
# executables: nothing in the compiler may shell out to an assembler, a C
# compiler, a linker, an archiver or a signing tool. This script fails when a file under
# Compiler/ names one of those programs, and when a file outside
# Compiler/System/ launches a process at all.
#
# No file may name a toolchain program, and only the ones listed below may
# launch a process at all. A file joins that list only with a reason written
# beside it.
#
# Run from anywhere: paths are resolved relative to the repository root.

set -eu

cd "$(dirname "$0")/../../.."

# Files still permitted to launch a process. Running a program is not the same
# as building one: both entries run what the compiler just produced.
#
#   Cli/CmdRun.cpp   directly runs the host artifact.
#   Cli/Testing/TestExecution.cpp  directly runs each executable target test artifact.
launch_exceptions='^Compiler/Cli/CmdRun\.cpp:'
launch_exceptions="$launch_exceptions"'|^Compiler/Cli/Testing/TestExecution\.cpp:'

# A string literal naming a toolchain program. `prefix` is the target triple a
# cross tool carries — `aarch64-linux-gnu-gcc` is as much a compiler as `gcc` —
# and `version` the release suffix. The short names are assembler mnemonics and
# register names throughout CodeGen/, so they count only when the literal also
# carries a directory or an extension; the long ones read as nothing else.
prefix='([A-Za-z0-9_.]+-)*'
version='([-.]?[0-9]+)?'
long_tool='clang(\+\+)?|gcc|g\+\+|lld(-link)?|ld\.lld|llvm-[a-z0-9-]+'
long_tool="$long_tool"'|ranlib|libtool|xcrun|codesign|dlltool|armasm64|ml64|gas'
short_tool='cc|c\+\+|ld|as|ar|link|cl|lib'
toolchain_pattern='"[^"]*[/\\]'"$prefix"'('"$long_tool"'|'"$short_tool"')'"$version"'(\.exe)?"'
toolchain_pattern="$toolchain_pattern"'|"'"$prefix"'('"$long_tool"')'"$version"'(\.exe)?"'
toolchain_pattern="$toolchain_pattern"'|"'"$prefix"'('"$short_tool"')'"$version"'\.exe"'

# The System layer's two process entry points. Everything that starts a program
# goes through one of them, so guarding these two names guards every spawn.
launch_pattern='\b(RunInherited|RunCaptured) *\('

named=$(grep -rnE "$toolchain_pattern" Compiler --include='*.cpp' --include='*.h' || true)

if [ -n "$named" ]; then
    echo "error: an external assembler, compiler, linker, archiver or signing tool is named in Compiler/ —" >&2
    echo "       Rux emits its own objects and links its own executables:" >&2
    echo "$named" >&2
    exit 1
fi

launched=$(grep -rnE "$launch_pattern" Compiler --include='*.cpp' --include='*.h' |
    grep -v '^Compiler/System/' | grep -vE "$launch_exceptions" || true)

if [ -n "$launched" ]; then
    echo "error: a process is launched outside Compiler/System/ and outside the files allowed to —" >&2
    echo "       if this is a build tool, it does not belong in the compiler at all:" >&2
    echo "$launched" >&2
    exit 1
fi

echo "External toolchain check passed."
