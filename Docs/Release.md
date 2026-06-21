# Release Pipeline

How a version tag becomes a published, multi-platform GitHub Release. The
pipeline is defined in [`.github/workflows/release.yml`](../.github/workflows/release.yml)
and is independent of the per-push CI in [CI/CD Flow](CI-CD.md).

## Trigger

A release is cut by pushing a `v*` tag:

```sh
git tag v0.3.1
git push origin v0.3.1
```

The workflow runs on `push: tags: [ 'v*' ]` and needs `contents: write`
permission to create the release.

## Stages

```
verify-version ──► linux  ──┐
                  windows ──┤──► release (publish)
                    macos ──┘
```

### 1. `verify-version` (fail fast)

Parses `VERSION` from `CMakeLists.txt` and compares it to the pushed tag
(minus the `v`). If they don't match, the run fails immediately — a mismatched
tag can never produce a release.

> **Always bump `VERSION` in `CMakeLists.txt` before tagging, and tag the exact
> same version.**

### 2. Build jobs (`linux`, `windows`, `macos`)

Each `needs: verify-version`, then builds Release **and runs the test suite** —
a broken build or failing test blocks the release. Each uploads its binary as an
artifact:

| Job       | Runner       | Artifact    |
|-----------|--------------|-------------|
| `linux`   | ubuntu-24.04 | `rux`       |
| `windows` | windows-2025 | `rux.exe`   |
| `macos`   | macos-26     | `rux-macos` |

### 3. `release` (publish)

`needs: [ linux, windows, macos ]`:

1. Download all build artifacts.
2. Package them:
    - `rux-linux.tar.gz` (preserves the executable bit)
    - `rux-macos.tar.gz` (staged back to plain `rux`)
    - `rux-windows.zip`
3. Create a **draft** GitHub Release with auto-generated release notes and the
   archives attached.

## Cutting a release — checklist

1. Ensure `dev` is green and promoted to `main` (see
   [Branch Architecture](Branches.md)).
2. Bump `VERSION` in `CMakeLists.txt`.
3. Update [`CHANGELOG.md`](../CHANGELOG.md).
4. Commit on `main`.
5. Tag `vX.Y.Z` matching the new `VERSION` and push the tag.
6. Wait for the pipeline; then review and **publish the draft release**.

> _TODO: confirm the post-build manual step — the release is created as a
> `draft`, so document who reviews and publishes it, and whether release notes
> are edited by hand._

## Versioning

> _TODO: state the versioning scheme (e.g. SemVer), what counts as a
> major/minor/patch change for a pre-1.0 language, and any tag conventions
> (pre-release suffixes like `v0.3.0-rc.1`)._

## Distribution

> _TODO: document where published binaries flow next — install scripts, package
> managers (Scoop, Homebrew, distro packages), and how the website Download page
> picks them up._
