#!/usr/bin/env sh
# Platform-isolation guard.
#
# Direct operating-system API usage (Win32 calls, POSIX process/console calls,
# environment access, ...) must stay inside Compiler/System/ so the rest of
# the compiler stays platform-agnostic. This script greps the rest of the tree
# for known OS entry points and fails when it finds one. If you need one of
# these facilities, add or reuse a wrapper in System/ (see System/Os.h and
# System/Process.h); if the System layer grows a new wrapper, extend the
# pattern below so the wrapped API stays fenced in.
#
# Run from anywhere: paths are resolved relative to the repository root.

set -eu

cd "$(dirname "$0")/../../.."

pattern='<windows\.h>|System/WinApi\.h'
pattern="$pattern"'|\bgetenv\b|_dupenv_s|GetEnvironmentVariable'
pattern="$pattern"'|\bisatty\b|\bioctl\b|GetConsole|SetConsoleMode|GetStdHandle *\('
pattern="$pattern"'|CreateProcess[AW]? *\(|\b_?popen *\(|\bfork *\('
pattern="$pattern"'|getrusage|GetProcessMemoryInfo|temp_directory_path'

violations=$(grep -rnE "$pattern" Compiler --include='*.cpp' --include='*.h' | grep -v '^Compiler/System/' || true)

if [ -n "$violations" ]; then
    echo "error: direct OS API usage outside Compiler/System/ — route it through the System layer:" >&2
    echo "$violations" >&2
    exit 1
fi

echo "Platform isolation check passed."
