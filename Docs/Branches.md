# Branch Architecture

How branches are organized in the Rux repository and the rules that govern them.

## The two long-lived branches

| Branch     | Role                                                             | Accepts PRs? |
| ---------- | ---------------------------------------------------------------- | ------------ |
| **`main`** | Release branch. Only release commits and version tags land here. | **No**       |
| **`dev`**  | Active development. Integration branch for all ongoing work.     | **Yes**      |

Both branches are built and tested by CI on every push and pull request (see
[CI/CD Flow](CI-CD.md)).

## Topic branches

Contributors branch off `dev` for each unit of work and open a PR back into
`dev`:

```sh
git checkout dev
git pull
git checkout -b feat/my-feature
```

### Naming convention

Topic branches **should** use a `type/short-description` prefix so the branch's
intent is clear at a glance. This is a recommendation, not a hard gate — but
following it keeps the branch list readable:

| Prefix      | Use for                                              |
| ----------- | --------------------------------------------------- |
| `feat/`     | New features or language capabilities               |
| `fix/`      | Bug fixes                                            |
| `docs/`     | Documentation-only changes                          |
| `refactor/` | Internal restructuring with no behavior change      |
| `chore/`    | Build, CI, tooling, and housekeeping                |

Use a short, hyphenated description after the prefix, e.g. `fix/lookup-overload`
or `feat/expand-rux-info`.

## How changes flow

```
topic branch ──PR──► dev ──(merge to main)──► main ──tag vX.Y.Z──► Release Pipeline
```

1. Work happens on topic branches and merges into `dev`.
2. When `dev` is ready to ship, it is promoted to `main` (see below).
3. A version tag on `main` triggers the [Release Pipeline](Release.md).

### Promoting `dev` to `main`

At release time, `dev` is merged into `main` with a **merge commit** (not a
fast-forward), so each release sits on a clear merge boundary in `main`'s
history. Only the **repository owner** promotes `dev` to `main` and pushes the
version tag that cuts the release. See the
[Release Pipeline](Release.md) for the full release checklist.

## Branch protection

Both `main` and `dev` are protected. Merging into either requires:

- **Passing CI status checks** — the Linux and Windows build/test jobs must be
  green before a PR can merge (see [CI/CD Flow](CI-CD.md)).
- **At least one approving review** before merge.

`main` additionally only receives changes through the owner-driven promotion
described above — ordinary contributions never target it directly.

## Tags

Releases are cut by pushing a `v*` tag whose version **must** match the
`VERSION` in `CMakeLists.txt` — CI rejects a mismatch. See the
[Release Pipeline](Release.md).
