# Shared human-output helpers for the POSIX repository entry points.
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
step() {
    printf '\n==> %s\n' "$1"
}
passed() {
    if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
        printf '\033[32m[PASSED]\033[0m %s\n' "$1"
    else
        printf '[PASSED] %s\n' "$1"
    fi
}
