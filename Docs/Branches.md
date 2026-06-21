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
git checkout -b my-feature
```

> _TODO: define a branch naming convention if you want one, e.g.
> `feat/…`, `fix/…`, `docs/…`._

## How changes flow

```
topic branch ──PR──► dev ──(release)──► main ──tag vX.Y.Z──► Release Pipeline
```

1. Work happens on topic branches and merges into `dev`.
2. When `dev` is ready to ship, its state is promoted to `main`.
3. A version tag on `main` triggers the [Release Pipeline](Release.md).

> _TODO: document exactly how `dev` is promoted to `main` (fast-forward, merge
> commit, or release PR) and who is allowed to do it._

## Branch protection

> _TODO: record the actual GitHub branch protection settings, e.g.:_
>
> - _Required passing status checks before merge (which workflows)_
> - _Required reviews / approvals_
> - _Whether `main` is push-restricted to maintainers_
> - _Linear history / no force-push rules_

## Tags

Releases are cut by pushing a `v*` tag whose version **must** match the
`VERSION` in `CMakeLists.txt` — CI rejects a mismatch. See the
[Release Pipeline](Release.md).
