# Pull Request Lifecycle

What happens to a change from the moment you open a pull request until it merges
into `dev`.

## Before you open a PR

- Your branch is based on `dev` and targets `dev` (see
  [Branch Architecture](Branches.md)).
- The build passes locally and the suite is green (`./Bin/Release/rux test`).
- Touched files are `clang-format`-clean.
- New behavior has a matching test package under `Tests/`.

## 1. Open the PR

- Target branch: **`dev`**.
- Keep it focused — one logical change per PR.
- Reference the relevant issue in the description (e.g. `Fixes #42`).
- Describe **what** changed and **why**, not just how.

There is no pull-request template in `.github/` today, so there's no enforced
checklist — write the description by hand and cover the points above. The
contributor expectations live in [`CONTRIBUTING.md`](../CONTRIBUTING.md); a PR
that satisfies the **Before you open a PR** list above is in good shape.

## 2. Automated checks

Opening or updating a PR triggers the full per-OS CI matrix — every supported
platform builds and runs the test suite. See [CI/CD Flow](CI-CD.md) for the
exact workflows and platforms.

Not every workflow blocks merging. The **required** checks are:

- **Linux** (`linux.yml`)
- **Windows** (`windows.yml`)

The macOS, BSD, and Illumos workflows run too and are worth watching, but they are
**informational** — they won't block the merge button. Both required checks must
be green before a PR is eligible to merge.

## 3. Review

- **At least one approving review** is required before a PR can merge.
- **Any maintainer** can review, approve, and merge — there is no `CODEOWNERS`
  file routing specific paths to specific people.
- Anyone is welcome to review and comment; only a maintainer's approval counts
  toward the requirement.
- Address feedback by pushing follow-up commits and re-requesting review, rather
  than force-pushing over the discussion (see [Etiquette](#etiquette)). If a
  review stalls, a polite nudge on the PR or in
  [Discord](https://discord.com/invite/uvSHjtZSVG) is fine.

## 4. Merge

- **Strategy: merge commit.** A maintainer merges the PR into `dev` with a merge
  commit, preserving the branch's commits — the same style used when `dev` is
  later promoted to `main`.
- **Who merges:** any maintainer, once the required checks are green and the PR
  has its approving review.
- **After merge the head branch is deleted** automatically; you can safely
  delete your local copy too (`git branch -d <branch>`).

## 5. After merge

- The change rides `dev` until the next promotion to `main`.
- It ships to users when a release is cut (see
  [Release Pipeline](Release.md)).

## Etiquette

- Keep PRs small and reviewable.
- Respond to review comments rather than silently force-pushing over them.
- Rebase on the latest `dev` if your PR falls behind.
- For work-in-progress, open a **draft PR** to get early CI signal and feedback
  before requesting a formal review; mark it ready when it's done.
- Avoid long stacks of interdependent PRs — prefer one focused, self-contained
  change. If a change genuinely must be split, land the base PR first and note
  the dependency in the follow-up's description.
