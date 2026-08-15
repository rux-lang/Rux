# Release Pipeline

How a version tag becomes a published, multi-platform GitHub Release. The pipeline is defined in [`.github/workflows/Release.yml`](../.github/workflows/Release.yml) and is independent of the per-push CI in [CI/CD Flow](CI-CD.md).

## Trigger

A release is cut by pushing a `v*` tag:

```sh
git tag v0.3.0
git push origin v0.3.0
```

The workflow runs on `push: tags: [ 'v*' ]` and needs `contents: write` permission to create the release.

## Stages

```
verify-version ──► freebsd ──► freebsd-cross-build ──► freebsd-cross-runtime ──► freebsd-acceptance ──┐
                   linux   ─────────────────────────────────────────────────────────────────────────────┤
                   macos   ─────────────────────────────────────────────────────────────────────────────┼──► release (publish)
                   windows ─────────────────────────────────────────────────────────────────────────────┘
```

### 1. `verify-version` (Fail Fast)

Parses `VERSION` from `CMakeLists.txt` and compares it to the pushed tag (minus the `v`). If they don't match, the run fails immediately — a mismatched tag can never produce a release.

> **Always bump `VERSION` in `CMakeLists.txt` before tagging, and tag the exact same version.**

### 2. Build Jobs (`freebsd`, `linux`, `macos`, `windows`)

Each `needs: verify-version`, then builds Release **and runs the test suite** — a broken build or failing test blocks the release. Each job has x86-64 and AArch64 matrix entries and uploads one architecture-labelled binary artifact. The Windows x86-64 entry additionally packages the freshly built binary into a per-user MSI installer (`Packaging/Windows/Msi/Build.ps1`):

| Job       | Runners                            | Binary artifacts                            |
| --------- | ---------------------------------- | ------------------------------------------- |
| `freebsd` | FreeBSD 14.4 x86-64/AArch64 VMs    | `rux-freebsd-x86_64`, `rux-freebsd-aarch64` |
| `linux`   | Ubuntu 24.04 x86-64/AArch64        | `rux-linux-x86_64`, `rux-linux-aarch64`     |
| `macos`   | macOS 26 Intel/Apple Silicon       | `rux-macos-x86_64`, `rux-macos-aarch64`     |
| `windows` | Windows 2025 x86-64/Windows 11 ARM | `rux-windows-x86_64`, `rux-windows-aarch64` |

The additional `rux-windows-x86_64-msi` workflow artifact carries the x86-64 MSI into the publish job. An AArch64 MSI is not produced because the current WiX package is authored for x64; AArch64 Windows is distributed as a ZIP.

Each job runs language tests from the repository root. Test manifests use local path dependencies, and transitive first-party dependencies resolve from workspace members with registry fallback disabled, so release validation is deterministic and network-independent. The Windows AArch64 entry additionally runs the native executable exit-code, assertion/panic output, and DLL load/call/unload fixtures before packaging. The macOS 26 AArch64 entry runs the Apple Silicon import-free exit-code, fixed/variadic libSystem, assertion/panic, and dylib load/call/unload fixtures; each image's ARM64 header and in-process ad-hoc signature are checked before execution. The FreeBSD jobs run the complete `Test.sh` workflow, and the AArch64 entry adds its freestanding, libc, assertion/panic, BSD syscall, and shared-library fixtures. These native acceptance steps must pass before the corresponding AArch64 asset can reach the publish job. The Linux job also checks every maintained Rux source with `rux fmt --check` before packaging.

### 3. FreeBSD transferred-cross acceptance

After both native FreeBSD matrix entries pass, `freebsd-cross-build` downloads the x86-64 compiler and builds a sealed AArch64 payload in an x86-64 FreeBSD 14.4 VM. `freebsd-cross-runtime` downloads only that payload into a fresh AArch64 FreeBSD VM, verifies its hashes, modes, and ELF identities, and launches the representative executables and shared-library fixture. The consumer VM installs no compiler or target toolchain.

`freebsd-acceptance` runs even when an upstream job fails or is skipped and checks both dependency results explicitly. This gives the publish job a single release-blocking FreeBSD gate: the AArch64 archive cannot be packaged merely because one of the two runtime paths did not run.

### 4. `release` (publish)

`needs: [ freebsd-acceptance, linux, macos, windows ]`:

1. Download all build artifacts.
2. Package them:
   - `rux-{freebsd,linux,macos}-{x86_64,aarch64}.tar.gz` (preserve the executable bit)
   - `rux-windows-{x86_64,aarch64}.zip`
   - `rux-windows-x86_64.msi`
   - the existing architecture-unqualified filenames as x86-64 compatibility aliases
   - `SHA256SUMS` (integrity hashes for every package)
3. Create a **draft** GitHub Release with auto-generated release notes and every package attached.

## Cutting a Release — Checklist

1. Ensure `dev` is green and promoted to `main` (see [Branch Architecture](Branches.md)).
2. Bump `VERSION` in `CMakeLists.txt`.
3. Update [`CHANGELOG.md`](../CHANGELOG.md).
4. Confirm the FreeBSD, Linux, macOS, and Windows required checks are green on `main`, including FreeBSD native and transferred-cross AArch64 acceptance.
5. Commit the version and changelog updates on `main`.
6. Create an annotated tag matching the new `VERSION`, then push it:
   ```sh
   git tag -a vX.Y.Z -m "Rux vX.Y.Z"
   git push origin vX.Y.Z
   ```
7. Wait for `verify-version`, both architecture entries of every platform job, and the FreeBSD transferred-cross runtime job to succeed.
8. A repository owner or maintainer with release permission reviews the draft:
   - Confirm the generated notes cover the intended changelog entries.
   - Download each asset and verify its filename and SHA-256 hash against `SHA256SUMS`.
   - Smoke-test at least one archive per platform and the Windows MSI.
9. Edit the generated notes if needed, then **publish the draft release**.

## Versioning

Rux follows [Semantic Versioning](https://semver.org/) and records user-visible changes using [Keep a Changelog](https://keepachangelog.com/). Tags are the project version prefixed with `v`.

While Rux is pre-1.0:

- **Minor** (`0.X.0`) releases may include intentional language, ABI, manifest, or CLI incompatibilities. Migration notes belong in `CHANGELOG.md`.
- **Patch** (`0.X.Y`) releases contain compatible bug fixes, documentation, and packaging improvements.
- **Pre-release** tags use SemVer suffixes such as `v0.4.0-rc.1`. The current version parser in the release workflow accepts only the numeric version from `CMakeLists.txt`, so pre-release automation must be updated before using such a tag.

Do not move or reuse a published tag. If a released artifact is defective, prepare a new patch version.

## Distribution

Canonical release asset names use `rux-<os>-<architecture>.<extension>`:

| Assets                              | Architectures       |
| ----------------------------------- | ------------------- |
| `rux-freebsd-<architecture>.tar.gz` | x86-64 and AArch64  |
| `rux-linux-<architecture>.tar.gz`   | x86-64 and AArch64  |
| `rux-macos-<architecture>.tar.gz`   | x86-64 and AArch64  |
| `rux-windows-<architecture>.zip`    | x86-64 and AArch64  |
| `rux-windows-x86_64.msi`            | x86-64 only         |
| `SHA256SUMS`                        | every release asset |

The architecture identifiers in filenames are `x86_64` and `aarch64`, matching compiler target and CI artifact identifiers. The release tag already carries the version, so asset names deliberately omit it; this also keeps stable GitHub `releases/latest/download/<asset>` URLs.

For compatibility, the workflow also publishes the former architecture-unqualified names (`rux-freebsd.tar.gz`, `rux-linux.tar.gz`, `rux-macos.tar.gz`, `rux-windows.zip`, and `rux-windows.msi`) as x86-64 aliases. The Linux and PowerShell installers consume the architecture-qualified assets.

The Linux and PowerShell installers use GitHub's `releases/latest/download/<asset>` redirect unless the user pins a version. Publishing the draft therefore makes the new release available to those installers without another repository change. The website's install endpoints serve the scripts; they do not host the release binaries themselves.

## Failed Releases

- A failed workflow before publication creates no public release. Fix the underlying problem, delete the unpublished tag if appropriate, and create a new tag only after the commit is ready.
- A draft with incorrect notes or attachments can be edited or deleted before publication.
- Never replace assets on an already published version silently. Publish a new patch release and document the correction in the changelog.
