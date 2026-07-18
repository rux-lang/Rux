# Release Pipeline

How a version tag becomes a published, multi-platform GitHub Release. The pipeline is defined in [`.github/workflows/release.yml`](../.github/workflows/release.yml) and is independent of the per-push CI in [CI/CD Flow](CI-CD.md).

## Trigger

A release is cut by pushing a `v*` tag:

```sh
git tag v0.3.0
git push origin v0.3.0
```

The workflow runs on `push: tags: [ 'v*' ]` and needs `contents: write` permission to create the release.

## Stages

```
verify-version ──► linux  ──┐
                  windows ──┴──► release (publish)
```

### 1. `verify-version` (fail fast)

Parses `VERSION` from `CMakeLists.txt` and compares it to the pushed tag (minus the `v`). If they don't match, the run fails immediately — a mismatched tag can never produce a release.

> **Always bump `VERSION` in `CMakeLists.txt` before tagging, and tag the exact same version.**

### 2. Build jobs (`linux`, `windows`)

Each `needs: verify-version`, then builds Release **and runs the test suite** — a broken build or failing test blocks the release. Each uploads its binary as an artifact; the Windows job additionally packages the freshly built binary into a per-user MSI installer (`Packaging/Windows/Msi/Build.ps1`):

| Job       | Runner       | Artifacts                                    |
| --------- | ------------ | -------------------------------------------- |
| `linux`   | ubuntu-24.04 | `rux-linux` (the `rux` binary)               |
| `windows` | windows-2025 | `rux-windows` (`rux.exe`), `rux-windows-msi` |

Before running language tests, each job executes bare `rux install` from the repository root. Manifest-less workspace discovery derives test dependencies from their manifests, so release validation uses the same dependency graph as normal CI and local development.

### 3. `release` (publish)

`needs: [ linux, windows ]`:

1. Download all build artifacts.
2. Package them:
   - `rux-linux.tar.gz` (preserves the executable bit)
   - `rux-windows.zip`
   - `rux-windows.msi` (attached as-is)
3. Create a **draft** GitHub Release with auto-generated release notes and the archives attached.

## Cutting a release — checklist

1. Ensure `dev` is green and promoted to `main` (see [Branch Architecture](Branches.md)).
2. Bump `VERSION` in `CMakeLists.txt`.
3. Update [`CHANGELOG.md`](../CHANGELOG.md).
4. Confirm the Linux and Windows required checks are green on `main`.
5. Commit the version and changelog updates on `main`.
6. Create an annotated tag matching the new `VERSION`, then push it:
   ```sh
   git tag -a vX.Y.Z -m "Rux vX.Y.Z"
   git push origin vX.Y.Z
   ```
7. Wait for `verify-version`, `linux`, and `windows` to succeed.
8. A repository owner or maintainer with release permission reviews the draft:
   - Confirm the generated notes cover the intended changelog entries.
   - Download each asset and verify its filename and size.
   - Smoke-test the Linux archive and at least one Windows installer.
9. Edit the generated notes if needed, then **publish the draft release**.

## Versioning

Rux follows [Semantic Versioning](https://semver.org/) and records user-visible changes using [Keep a Changelog](https://keepachangelog.com/). Tags are the project version prefixed with `v`.

While Rux is pre-1.0:

- **Minor** (`0.X.0`) releases may include intentional language, ABI, manifest, or CLI incompatibilities. Migration notes belong in `CHANGELOG.md`.
- **Patch** (`0.X.Y`) releases contain compatible bug fixes, documentation, and packaging improvements.
- **Pre-release** tags use SemVer suffixes such as `v0.4.0-rc.1`. The current version parser in the release workflow accepts only the numeric version from `CMakeLists.txt`, so pre-release automation must be updated before using such a tag.

Do not move or reuse a published tag. If a released artifact is defective, prepare a new patch version.

## Distribution

The release workflow publishes three user-facing assets:

| Asset              | Consumer                                                           |
| ------------------ | ------------------------------------------------------------------ |
| `rux-linux.tar.gz` | Linux installer script in `Packaging/Linux/install.sh`             |
| `rux-windows.zip`  | PowerShell installer in `Packaging/Windows/PowerShell/install.ps1` |
| `rux-windows.msi`  | Direct Windows MSI installation                                    |

The Linux and PowerShell installers use GitHub's `releases/latest/download/<asset>` redirect unless the user pins a version. Publishing the draft therefore makes the new release available to those installers without another repository change. The website's install endpoints serve the scripts; they do not host the release binaries themselves.

## Failed releases

- A failed workflow before publication creates no public release. Fix the underlying problem, delete the unpublished tag if appropriate, and create a new tag only after the commit is ready.
- A draft with incorrect notes or attachments can be edited or deleted before publication.
- Never replace assets on an already published version silently. Publish a new patch release and document the correction in the changelog.
