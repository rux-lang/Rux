# Release Pipeline

How a dispatched run becomes a published, multi-platform GitHub Release. The pipeline is defined in [`.github/workflows/Release.yml`](../.github/workflows/Release.yml) and builds every target by calling the same workflows the per-push CI in [CI/CD Flow](CI-CD.md) runs.

## Trigger

A release is cut by hand: open the **Release** workflow under Actions, choose the `main` branch, enter the version without the leading `v`, and run it. Pushing a tag never starts a release by itself, so there is nothing to race and nothing to cancel by accident. The workflow creates the tag.

```sh
gh workflow run Release.yml --ref main -f version=0.4.0
gh workflow run Release.yml --ref main -f version=0.4.0 -f dry-run=true   # rehearse without tagging
```

`dry-run: true` performs everything except the tag and the draft, so a release can be rehearsed in full.

## Stages

```
verify ──► linux-x86_64, linux-aarch64, macos-aarch64, macos-x86_64,        ──► publish ──┐
           windows-x86_64, windows-aarch64, freebsd-x86_64, freebsd-aarch64                ├──► draft
           windows-x86_64 ──────────────────────────────────────────────────► installer ──┘
```

### 1. `verify` (Fail Fast)

Checks that the requested version is a three-part number, that `project(Rux VERSION ...)` in `CMakeLists.txt` declares exactly it, that `CHANGELOG.md` has a `## [version]` section, and that the tag `v<version>` does not already exist. If any check fails, the run ends before a single target builds.

> **Always bump the version in `CMakeLists.txt` and add its changelog section before dispatching, and dispatch the exact same version.**

### 2. Target builds

Each of the eight jobs `needs: verify` and calls the target's reusable workflow with the `extended` scope, so a release is built by the very steps CI verified and nothing that runs under emulation is skipped: every target builds Release **and runs its suites**, and a broken build or failing test blocks the release.

| Job               | Runner                                          | Binary artifact       |
| ----------------- | ----------------------------------------------- | --------------------- |
| `linux-x86_64`    | Ubuntu 26.04                                    | `rux-linux-x86_64`    |
| `linux-aarch64`   | Ubuntu 26.04 ARM                                | `rux-linux-aarch64`   |
| `macos-aarch64`   | macOS 26                                        | `rux-macos-aarch64`   |
| `macos-x86_64`    | macOS 26, cross-built and tested under Rosetta  | `rux-macos-x86_64`    |
| `windows-x86_64`  | Windows 2025, then Windows 11 ARM under emulation | `rux-windows-x86_64` |
| `windows-aarch64` | Windows 11 ARM                                  | `rux-windows-aarch64` |
| `freebsd-x86_64`  | FreeBSD 15.1 x86-64 guest, then the transfer test in an AArch64 guest | `rux-freebsd-x86_64` |
| `freebsd-aarch64` | cross-built in the x86-64 guest, then the complete suites in an AArch64 guest | `rux-freebsd-aarch64` |

Each job runs language tests from the repository root. Test manifests use local path dependencies, and transitive first-party dependencies resolve from workspace members with registry fallback disabled, so release validation is deterministic and network-independent. The Windows AArch64 job additionally runs the native executable exit-code, assertion/panic output, stack-probe, and DLL load/call/unload fixtures. The macOS AArch64 job runs the Apple Silicon import-free exit-code, fixed/variadic libSystem, assertion/panic, and dylib load/call/unload fixtures; each image's ARM64 header and in-process ad-hoc signature are checked before execution. The FreeBSD AArch64 job runs its freestanding, libc, assertion/panic, BSD syscall, and shared-library fixtures on a native AArch64 kernel, and the FreeBSD x86-64 job's transfer test has a fresh AArch64 guest with no compiler installed verify and execute a payload the x86-64 compiler cross-built. These native acceptance steps must pass before the corresponding asset can reach the publish job. The Linux x86-64 job also checks every maintained Rux source with `rux fmt --check`.

### 3. `publish` and `installer`

`publish` needs all eight target jobs, downloads their `rux-<target>` artifacts, and packages them:

- `rux-{freebsd,linux,macos}-{x86_64,aarch64}.tar.gz` (preserving the executable bit)
- `rux-windows-{x86_64,aarch64}.zip`
- the architecture-unqualified filenames as compatibility aliases

`installer` needs only `windows-x86_64` and packages the freshly built binary into a per-user MSI (`Packaging/Windows/Msi/Build.ps1`), uploaded as `rux-windows.msi`. An AArch64 MSI is not produced because the current WiX package is authored for x64; AArch64 Windows is distributed as a ZIP.

### 4. `draft`

Needs both. It merges the payload and the MSI, writes `SHA256SUMS` over every asset, uploads the whole set as the `release-<version>` workflow artifact, and — unless this is a dry run — creates the tag and a **draft** GitHub Release with every asset attached and empty notes. It is the only job granted `contents: write`.

## Cutting a Release — Checklist

1. Ensure `dev` is green and promoted to `main` (see [Branch Architecture](Branches.md)).
2. Bump the version in `CMakeLists.txt`.
3. Add the version's section to [`CHANGELOG.md`](../CHANGELOG.md).
4. Commit the version and changelog updates on `main` and confirm the `CI` check is green there.
5. Dispatch **Release** on `main` with the version, first with `dry-run` if the last release is more than a few weeks old.
6. Wait for every target job, `publish`, `installer`, and `draft` to succeed.
7. A repository owner or maintainer with release permission reviews the draft:
   - Write the notes from the changelog section.
   - Download each asset and verify its filename and SHA-256 hash against `SHA256SUMS`.
   - Smoke-test at least one archive per platform and the Windows MSI.
8. **Publish the draft release.**

## Versioning

Rux follows [Semantic Versioning](https://semver.org/) and records user-visible changes using [Keep a Changelog](https://keepachangelog.com/). Tags are the project version prefixed with `v`.

While Rux is pre-1.0:

- **Minor** (`0.X.0`) releases may include intentional language, ABI, manifest, or CLI incompatibilities. Migration notes belong in `CHANGELOG.md`.
- **Patch** (`0.X.Y`) releases contain compatible bug fixes, documentation, and packaging improvements.
- **Pre-release** tags use SemVer suffixes such as `v0.4.0-rc.1`. The version check in the release workflow accepts only the numeric version from `CMakeLists.txt`, so pre-release automation must be updated before using such a tag.

Do not move or reuse a published tag. If a released artifact is defective, prepare a new patch version.

## Distribution

Canonical release asset names use `rux-<os>-<architecture>.<extension>`:

| Assets                              | Architectures       |
| ----------------------------------- | ------------------- |
| `rux-freebsd-<architecture>.tar.gz` | x86-64 and AArch64  |
| `rux-linux-<architecture>.tar.gz`   | x86-64 and AArch64  |
| `rux-macos-<architecture>.tar.gz`   | x86-64 and AArch64  |
| `rux-windows-<architecture>.zip`    | x86-64 and AArch64  |
| `rux-windows.msi`                   | x86-64 only         |
| `SHA256SUMS`                        | every release asset |

The architecture identifiers in filenames are `x86_64` and `aarch64`, matching compiler target and CI artifact identifiers. The release tag already carries the version, so asset names deliberately omit it; this also keeps stable GitHub `releases/latest/download/<asset>` URLs.

For compatibility, the workflow also publishes the former architecture-unqualified names (`rux-freebsd.tar.gz`, `rux-linux.tar.gz`, `rux-windows.zip` as x86-64 aliases, and `rux-macos.tar.gz` as the Apple Silicon alias). The Linux and PowerShell installers consume the architecture-qualified assets.

The Linux and PowerShell installers use GitHub's `releases/latest/download/<asset>` redirect unless the user pins a version. Publishing the draft therefore makes the new release available to those installers without another repository change. The website's install endpoints serve the scripts; they do not host the release binaries themselves.

## Failed Releases

- A failed run before `draft` creates no tag and no public release. Fix the underlying problem and dispatch again.
- A run that failed after tagging has left the tag but no published release: delete the tag and the draft, then dispatch again once the commit is ready.
- A draft with incorrect notes or attachments can be edited or deleted before publication.
- Never replace assets on an already published version silently. Publish a new patch release and document the correction in the changelog.
