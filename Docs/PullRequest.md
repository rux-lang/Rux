# Pull Request Lifecycle

What happens to a change from the moment you open a pull request until it merges
into `dev`.

## Before you open a PR

- Your branch is based on `dev` and targets `dev` (see
  [Branch Architecture](Branches.md)).
- The build passes locally and the suite is green (`./build/rux test`).
- Touched files are `clang-format`-clean.
- New behavior has a matching test package under `Tests/`.

## 1. Open the PR

- Target branch: **`dev`**.
- Keep it focused — one logical change per PR.
- Reference the relevant issue in the description (e.g. `Fixes #42`).
- Describe **what** changed and **why**, not just how.

> _TODO: link a PR template under `.github/` if/when one exists, and list any
> required checklist items._

## 2. Automated checks

Opening or updating a PR triggers the full per-OS CI matrix — every supported
platform builds and runs the test suite. See [CI/CD Flow](CI-CD.md) for the
exact workflows and platforms.

All required checks must be green before a PR is eligible to merge.

> _TODO: confirm which checks are *required* vs. informational in branch
> protection._

## 3. Review

> _TODO: document the review expectations, e.g.:_
> - _How many approvals are required_
> - _Who reviews (maintainers / CODEOWNERS)_
> - _Expected turnaround and how to nudge a stalled review_
> - _How to handle review feedback (push fixups, re-request review)_

## 4. Merge

> _TODO: document the merge strategy and who performs the merge, e.g.:_
> - _Squash / merge commit / rebase_
> - _Whether the branch is deleted after merge_
> - _Any "do not merge" labels or gates_

## 5. After merge

- The change rides `dev` until the next promotion to `main`.
- It ships to users when a release is cut (see
  [Release Pipeline](Release.md)).

## Etiquette

- Keep PRs small and reviewable.
- Respond to review comments rather than silently force-pushing over them.
- Rebase on the latest `dev` if your PR falls behind.

> _TODO: any project-specific norms (e.g. draft PRs for early feedback,
> conventions for stacked PRs)._
