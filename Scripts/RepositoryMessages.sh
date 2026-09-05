# Shared human-output helpers for the POSIX repository entry point.
die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}
run_checked() {
    command_name=$1
    shift
    if "$@"; then
        return 0
    else
        status=$?
    fi
    printf "error: command '%s' failed with exit code %s\n" "$command_name" "$status" >&2
    exit "$status"
}
# Print the first candidate found on PATH or as an executable file path.
find_tool() {
    for candidate in "$@"; do
        if resolved=$(command -v "$candidate" 2>/dev/null); then
            printf '%s' "$resolved"
            return 0
        fi
    done
    return 1
}
# Locate an LLVM 23 tool, preferring versioned names and Homebrew keg paths.
find_llvm_tool() {
    tool=$1
    find_tool "$tool-23" "${tool}23" \
        "/opt/homebrew/opt/llvm@23/bin/$tool" "/usr/local/opt/llvm@23/bin/$tool" \
        "/opt/homebrew/opt/llvm/bin/$tool" "/usr/local/opt/llvm/bin/$tool" \
        "$tool"
}
format_duration() {
    elapsed_seconds=$1
    if [ "$elapsed_seconds" -le 0 ]; then
        printf '0 ms'
    elif [ "$elapsed_seconds" -lt 60 ]; then
        printf '%s s' "$elapsed_seconds"
    else
        printf '%s min %s.0 s' "$((elapsed_seconds / 60))" "$((elapsed_seconds % 60))"
    fi
}
supports_color() {
    [ -t 1 ] && [ -z "${NO_COLOR:-}" ]
}
# Decided once at load: a helper evaluated inside a command substitution would see a pipe on its standard output.
if supports_color; then style_enabled=true; else style_enabled=false; fi
# A status verb rendered the way the compiler does: bold in the given ANSI color when styling is enabled.
status_verb() {
    if [ "$style_enabled" = true ]; then
        printf '\033[1;%sm%s\033[0m' "$2" "$1"
    else
        printf '%s' "$1"
    fi
}
step() {
    if supports_color; then
        printf '\n\033[36m==> %s\033[0m\n' "$1"
    else
        printf '\n==> %s\n' "$1"
    fi
}
passed() {
    printf '%s %s\n' "$(status_verb Passed 32)" "$1"
}
failed() {
    printf '%s %s\n' "$(status_verb Failed 31)" "$1"
}
finished() {
    if supports_color; then
        printf '\n\033[32m%s\033[0m\n' "$1"
    else
        printf '\n%s\n' "$1"
    fi
}
