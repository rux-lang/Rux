# Package Acceptance Evidence

What is actually on record for the twenty-five first-party packages, separated from what is merely expected to be true. [Publication readiness](Packages.md#publication-readiness) states the rule; this file states the evidence, so that the claim "the packages are ready" can be checked rather than believed.

Acceptance and completion are different states. Every task that built these packages is complete and locally verified. No phase is accepted, because acceptance requires an extended CI run pinned to the exact commit, and that run has never happened.

## What Acceptance Requires

| Criterion                                                    | State                | Settled by                                    |
| ------------------------------------------------------------ | -------------------- | --------------------------------------------- |
| Canonical `SourceLibrary` manifests, no path dependency       | On record            | This host                                     |
| `rux check` clean on all eight target cells                   | On record            | This host                                     |
| `rux doc` clean on all eight target cells, routes distinct    | On record            | This host                                     |
| Deterministic publishable archive                             | On record            | This host                                     |
| Every documentation example compiles                          | On record, automated | `Tests/Unit/PackageExampleTests.cpp`          |
| Documentation house style with an empty baseline              | On record, automated | `Tests/Unit/PackageDocumentationStyleTests.cpp` |
| Native behavior executed on all eight cells                   | **Not on record**    | The [CI matrix](CI-CD.md), never run at a phase commit |
| Extended CI run pinned to the phase commit                    | **Not on record**    | `CI.yml` with the `extended` scope             |
| Independent cryptographic review of `Rux/Crypto`              | **Not on record**    | [The review checklist](CryptoReview.md)        |
| `/storage/` documentation routes retired or redirected        | **Not on record**    | The separate website repository                |

## Evidence Recorded on This Host

Gathered at commit `cf7e67a9` on Windows 11 Pro 26200 x86-64, with `Rux 0.4.0 (2026-09-08 07:04:17 UTC)` built from that tree. The numbers below are what the commands reported, not what they were expected to report.

**Two hundred `rux check` runs, zero failures.** Twenty-five packages against each of `freebsd-aarch64`, `freebsd-x86_64`, `linux-aarch64`, `linux-x86_64`, `macos-aarch64`, `macos-x86_64`, `windows-aarch64` and `windows-x86_64`. A cell other than the host is compiled and analyzed but never executed, which is exactly the gap the CI matrix exists to close.

**Two hundred `rux doc` runs, zero failures.** The same grid. A whole-workspace run generates twenty-six pages — an index and one per package — with no duplicate route and no missing declaration.

**Fifty `rux pack` runs and twenty-five `rux publish --dry-run` runs, zero failures.** Each package was packed twice into separate paths and the two archives compared by SHA-256. All twenty-five pairs agree byte for byte.

**Twenty-five distinct documentation routes.** Every manifest's `Homepage` is `https://rux-lang.dev/docs/api/<name>`, all twenty-five distinct, and none is under the retired `/storage/` prefix. The route the rename moved to `/filesystem` is the one `Rux/FileSystem` declares.

### Archive Determinism

The digests belong to the tree at `cf7e67a9` and are expected to change whenever a package does. What is being recorded is not the numbers but the property they demonstrate: two packs of one tree produce the same bytes.

| Package     | Bytes   | SHA-256                                                            |
| ----------- | ------- | ------------------------------------------------------------------ |
| Algorithms  | 129,673 | `ae784ab05f860db40264d116a42639dc3bc5d6b67cbf3393c66a35c9a0cf4370` |
| Allocator   | 92,045  | `c355dcd2b1eebe6be7de85a91a1428769aec986ec96385061b75d77a532c1e0c` |
| C           | 138,091 | `197d42d99e3116b4992c9f6d720523a9de09e12ee21da960f9d8a97283be7216` |
| Collections | 289,618 | `10af987fd7d59fcd64a9c1d4eb59b50d6c1c6f4ed921d66898efd7163af48aa1` |
| Core        | 153,937 | `a570140cddaa265eafda06bb68c001a93df9a15e866abcc243d410eccfd1b370` |
| Crypto      | 103,672 | `59899d46f53302b05a079b009ef4e9651a9e14d0b935eba11f0f5edceb518db2` |
| Entropy     | 24,201  | `dcc4f2a6641148bbd5dc1f70c3aa6ebe84d461f4fffbf4ebc2f9f1a95bb38beb` |
| FileSystem  | 131,190 | `814f2598b1ba81f7e3e10d7d17269b471f72fd212d3f78f9b035767c7b676f28` |
| Format      | 301,012 | `a48637083ad0823d47da165216a51b125cef40e6b0e9dc045b20572918023fbf` |
| FreeBSD     | 65,773  | `c53c68ff5a97116e6de3f53dea3fd79e035dbd3c14769746c03dd0e447a3c975` |
| Hash        | 70,060  | `a542a56ffea6e25eafa631fec5913d9012f1393b029a9245f4f32fc0a658dce9` |
| Io          | 113,416 | `1ef30b5c0032e79501ffeebc25bbdddae949ced0387ba8ff941de5251815fa77` |
| Json        | 101,819 | `345f5ec67cddba0a8b62fb86485f98d51e06c3b38fe13bd105bf247c700d3578` |
| Linux       | 78,497  | `6bfb1217359c5b608dc6aa88f39fa1ac4efd57e10f244c06e43ca85d348b7ad9` |
| Math        | 167,969 | `f9961bfcbc8e18fa8fe0c484b5a82d3cf465294eafb284c73e6cb62986e2ad90` |
| Memory      | 55,694  | `b99dac0ab6be092171b5174691c3cc878818731028d1f5c3c4bd0bd79ea5538d` |
| Path        | 66,076  | `f721fc4b535db8be93828ca4a7e03fa757e92c5effef4c06d7e6d906ecd705d1` |
| Random      | 52,271  | `9d311eeb2daac484b42e41c252f393eeec30b7840c6cb7c3272b0d0081cbdadd` |
| Text        | 174,596 | `ebf8828b627d35ddd27d951dc3c1b3c8e89f2b419bed4157de1b207e2a970c97` |
| Time        | 115,583 | `71639316bd0142e4bfd4b90948bad665d8433b38f83848d52eed7ab628029d53` |
| Toml        | 159,545 | `36d8d10650ad1b1ea3ed7af11f43c880324a479a0ab909e08f93d60455558f52` |
| Unicode     | 677,071 | `337182024316eb7a1747f33b56ee82ec8699c01e258b7ce6e7cb6d040353f3b7` |
| Uuid        | 39,190  | `12eeb6188765b9771edaeb9b1313a0cfd635137d2583c427d8d175505ee082fc` |
| Windows     | 92,170  | `b78f58516a903311d9b5b61d05f9d1c618847cddb8dbb227834e6302e37f0c1b` |
| macOS       | 62,695  | `3d42b42019a5f07435184a7f2f8a7333189b03f5ef2e054f68e131a620e9e462` |

### Documentation

The house style is a rule rather than a ratchet: `Tests/Unit/PackageDocumentationStyleBaseline.txt` holds a comment header and nothing else, so a new violation fails outright. It once held 4,782 entries.

Every fenced `rux` block in every package README is compiled by `Tests/Unit/PackageExampleTests.cpp`: twenty-five compile, five are fragments that lean on names their surrounding prose supplies, and one illustrates a signature. That test exists because five examples were broken before anything looked at them, and no other check would have found any of them.

## What the Test Suite Proves on This Host

Three hundred and sixty-four package and language tests pass, and eighteen CTest entries pass. Read that number carefully, because it does not mean what it appears to mean for the three POSIX platform packages.

Nine test packages — `Tests/Packages/{Linux,macOS,FreeBSD}/{Descriptors,Mapping,Syscall}` — guard their body with `when #target.os` and fall through to a `Main` that returns zero on any other system. On a Windows host they pass **vacuously**: nothing in them ran, and a passing result says only that the file compiled. Their entire value is in what the Linux, macOS and FreeBSD jobs report, and those jobs have never reported it.

`Tests/Packages/Windows/{Constants,Failures,Platform}` is the contrast and the reason the distinction is worth drawing. Those run on the host that wrote them, so every sentinel, Win32 error code and `NTSTATUS` boundary in them was read out of a running program rather than out of documentation.

## What Is Not on Record

**The eight execution jobs.** Native behavior on Windows, Linux, macOS and FreeBSD across both architectures. The compiler refuses by design to run a foreign target's programs, so seven of the eight cells are compiled here and never executed. Until they run, the syscall numbers, flag values, errno constants and `struct stat` offsets in the four platform binding packages rest on published documentation.

**An extended CI run pinned to a phase commit.** `CI.yml` carries the `extended` scope on a push to `dev`, and `Release.yml` calls all eight target workflows with it. Neither has been run against a commit that a phase record names, so no phase can cite one.

**The independent cryptographic review.** `Rux/Crypto` is blocked twice over: once by the execution gap that blocks everything, and once by [the review checklist](CryptoReview.md), which its own README and digest contract already say is outstanding.

**The website routes.** Retiring or redirecting the `/storage/` documentation routes belongs to the separate website repository and is on its own schedule. Nothing in this repository can close it.

## Reproducing the Local Evidence

The package cache has to match the working tree first, or a check resolves a dependency from a stale copy and fails for a reason that is not in the source:

```sh
pwsh -File Scripts/SyncLocalPackages.ps1
```

Then the grid, which takes about a minute:

```sh
for package in Packages/*/; do
  for target in freebsd-aarch64 freebsd-x86_64 linux-aarch64 linux-x86_64 \
                macos-aarch64 macos-x86_64 windows-aarch64 windows-x86_64; do
    rux --manifest "$package/Rux.toml" check --target "$target" || echo "check $package $target"
    rux --manifest "$package/Rux.toml" doc --target "$target" --output "Temp/Docs/$target" || echo "doc $package $target"
  done
  rux --manifest "$package/Rux.toml" pack --output "Temp/A.ruxpkg"
  rux --manifest "$package/Rux.toml" pack --output "Temp/B.ruxpkg"
  cmp "Temp/A.ruxpkg" "Temp/B.ruxpkg" || echo "archive $package"
  rux --manifest "$package/Rux.toml" publish --dry-run || echo "publish $package"
done
```

The documentation style and example checks need no separate command; they are part of the unit suite:

```sh
pwsh -File Run.ps1 test
```
