#!/bin/sh

# Decide how much verification a run gets.
#
#   sh .github/Scripts/Scope.sh classify < changed-files    docs | code
#   sh .github/Scripts/Scope.sh tier                        fast | full | extended
#   sh .github/Scripts/Scope.sh resolve                     docs | fast | full | extended
#
# classify reads one path per line and answers docs only when every path is
# documentation or community metadata; an empty list is code, because a run
# that cannot tell what changed must verify everything. tier reads the event
# CI.yml exports through the environment: a requested scope wins, a dispatched
# run is extended, so are pushes to dev and main, a pull request is full, and
# any other push is the fast lane. resolve combines the two, asking the GitHub
# API which files a push or pull request changed; when it cannot, the tier
# stands, never a smaller one.

set -eu

die() {
    printf 'error: %s\n' "$1" >&2
    exit 1
}

classify() {
    seen=0
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        seen=$((seen + 1))
        # A * in a case pattern spans directory separators, so Docs/*.md is
        # every Markdown file under Docs/ and .github/*.md every one under
        # .github/. A *.md anywhere else, such as a package license or the
        # test-suite guide, is code: something may read it.
        case "$path" in
        Docs/*.md | .github/*.md | .github/ISSUE_TEMPLATE/* | .github/FUNDING.yml) ;;
        */*)
            printf 'code\n'
            return 0
            ;;
        *.md) ;;
        *)
            printf 'code\n'
            return 0
            ;;
        esac
    done
    if [ "$seen" -eq 0 ]; then
        printf 'code\n'
    else
        printf 'docs\n'
    fi
}

tier() {
    if [ -n "${RUX_REQUESTED_SCOPE:-}" ]; then
        case "$RUX_REQUESTED_SCOPE" in
        fast | full | extended) printf '%s\n' "$RUX_REQUESTED_SCOPE" ;;
        *) die "unknown scope '$RUX_REQUESTED_SCOPE'; use fast, full, or extended" ;;
        esac
        return 0
    fi
    case "${GITHUB_EVENT_NAME:-}" in
    workflow_dispatch) printf 'extended\n' ;;
    pull_request) printf 'full\n' ;;
    push)
        case "${GITHUB_REF_NAME:-}" in
        dev | main) printf 'extended\n' ;;
        *) printf 'fast\n' ;;
        esac
        ;;
    *) printf 'full\n' ;;
    esac
}

resolve() {
    tier=$(tier)
    case "${GITHUB_EVENT_NAME:-}" in
    pull_request)
        base=${RUX_PR_BASE_SHA:-}
        head=${RUX_PR_HEAD_SHA:-}
        ;;
    push)
        base=${RUX_EVENT_BEFORE:-}
        head=${GITHUB_SHA:-}
        ;;
    *)
        printf '%s\n' "$tier"
        return 0
        ;;
    esac
    # A new branch pushes from the all-zero commit, which nothing can compare.
    case "$base" in
    '' | 0000000000000000000000000000000000000000)
        printf '%s\n' "$tier"
        return 0
        ;;
    esac
    if [ -z "$head" ]; then
        printf '%s\n' "$tier"
        return 0
    fi
    if ! changed=$(gh api "repos/${GITHUB_REPOSITORY:?}/compare/$base...$head" \
        --paginate --jq '.files[].filename' 2>/dev/null); then
        printf 'warning: could not list the changed files; verifying everything\n' >&2
        printf '%s\n' "$tier"
        return 0
    fi
    if [ "$(printf '%s\n' "$changed" | classify)" = docs ]; then
        printf 'docs\n'
    else
        printf '%s\n' "$tier"
    fi
}

case "${1:-}" in
classify) classify ;;
tier) tier ;;
resolve) resolve ;;
*) die 'usage: sh .github/Scripts/Scope.sh classify|tier|resolve' ;;
esac
