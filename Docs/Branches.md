# Branch Architecture

How branches are organized in the Rux repository and the rules that govern them.

## The Two Long-Lived Branches

| Branch     | Role                                                                    | Accepts PRs? |
| ---------- | ----------------------------------------------------------------------- | ------------ |
| **`main`** | Release branch. Receives tested promotions from `dev` and version tags. | **No**       |
| **`dev`**  | Active development. Integration branch for all ongoing work.            | **Yes**      |

Both branches are built and tested by CI on every push and pull request (see [CI/CD Flow](CI-CD.md)).

## Topic Branches

Contributors branch off `dev` for each unit of work and open a PR back into `dev`:

```sh
git switch dev
git pull --ff-only
git switch -c feat/my-feature
```

### Naming Convention

Topic branches **should** use a `type/short-description` prefix, so the branch's intent is clear at a glance. This is a recommendation, not a hard gate — but following it keeps the branch list readable:

| Prefix      | Use for                                        |
| ----------- | ---------------------------------------------- |
| `feat/`     | New features or language capabilities          |
| `fix/`      | Bug fixes                                      |
| `docs/`     | Documentation-only changes                     |
| `refactor/` | Internal restructuring with no behavior change |
| `chore/`    | Build, CI, tooling, and housekeeping           |

Use a short, hyphenated description after the prefix, e.g. `fix/lookup-overload` or `feat/expand-rux-info`.

Keep unrelated work on separate branches. If `dev` advances while you are working, merge or rebase it before requesting final review; avoid rewriting commits after review has started unless the reviewer agrees.

## How Changes Flow

```
topic branch ──PR──► dev ──(merge to main)──► main ──dispatch Release──► tag vX.Y.Z + draft
```

1. Work happens on topic branches and merges into `dev`.
2. When `dev` is ready to ship, it is promoted to `main` (see below).
3. The [Release Pipeline](Release.md) is dispatched by hand on `main`; it creates the version tag.

### Promoting `dev` to `main`

At release time, `dev` is merged into `main` with a **merge commit** (not a fast-forward), so each release sits on a clear merge boundary in `main`'s history. Only the **repository owner** promotes `dev` to `main` and dispatches the release that cuts the version tag. See the [Release Pipeline](Release.md) for the full release checklist.

After promotion, new development continues from `dev`; do not base ordinary feature work on `main`.

## Branch Protection

Both `main` and `dev` are protected. Merging into either requires:

- **The `CI` status check** — the single check that aggregates every job of the run, so every selected target must be green before a PR can merge (see [CI/CD Flow](CI-CD.md)).
- **At least one approving review** before merge.

`main` additionally only receives changes through the owner-driven promotion described above — ordinary contributions never target it directly.

## Tags

Tags are created by the [Release Pipeline](Release.md), never by hand: it is dispatched with a version that **must** match the one in `CMakeLists.txt` and have a changelog section, and it rejects a mismatch or an existing tag. Published tags are immutable; corrections ship as a new patch version rather than by moving an existing tag.
