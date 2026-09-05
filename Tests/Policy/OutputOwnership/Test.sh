#!/usr/bin/env sh
set -eu
script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/rux-output-ownership.XXXXXX")
trap 'rm -rf "$fixture_root"' EXIT HUP INT TERM
mkdir -p "$fixture_root/Compiler/Cli" "$fixture_root/Compiler/Stage" "$fixture_root/Tests/Policy/OutputOwnership"
cp "$script_directory/Exceptions.txt" "$fixture_root/Tests/Policy/OutputOwnership/Exceptions.txt"
printf '%s\n' 'void Render() { std::println("status"); }' >"$fixture_root/Compiler/Cli/Output.cpp"
printf '%s\n' 'auto Analyze() { return "Warning: 12 ms"; }' >"$fixture_root/Compiler/Stage/Analyze.cpp"
sh "$script_directory/Check.sh" "$fixture_root" >"$fixture_root/output.txt" 2>&1
printf '%s\n' 'void Analyze() { std::println("status"); }' >"$fixture_root/Compiler/Stage/Analyze.cpp"
if sh "$script_directory/Check.sh" "$fixture_root" >"$fixture_root/output.txt" 2>&1; then
    printf 'error: output ownership accepted a stage writing process output\n' >&2
    exit 1
fi
grep -F 'Compiler/Stage/Analyze.cpp:1: error:' "$fixture_root/output.txt" >/dev/null
printf '%s\n' 'Compiler/Stage/Analyze.cpp|A test inspection payload.' >>"$fixture_root/Tests/Policy/OutputOwnership/Exceptions.txt"
sh "$script_directory/Check.sh" "$fixture_root" >"$fixture_root/output.txt" 2>&1
printf 'Output ownership fixture tests passed.\n'
