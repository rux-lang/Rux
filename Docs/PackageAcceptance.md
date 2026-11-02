# Package Acceptance Evidence

What is actually on record for the thirty-nine first-party packages, separated from what is merely expected to be true. [Publication readiness](Packages.md#publication-readiness) states the rule; this file states the evidence, so that the claim "the packages are ready" can be checked rather than believed.

Acceptance and completion are different states. Every task that built these packages is complete and locally verified. No phase is accepted, because acceptance requires an extended CI run pinned to the exact commit, and that run has never happened.

## What Acceptance Requires

| Criterion                                                  | State                | Settled by                                             |
| ---------------------------------------------------------- | -------------------- | ------------------------------------------------------ |
| Canonical `SourceLibrary` manifests, no path dependency    | On record            | This host                                              |
| `rux check` clean on all eight target cells                | On record            | This host                                              |
| `rux doc` clean on all eight target cells, routes distinct | On record            | This host                                              |
| Deterministic publishable archive                          | On record            | This host                                              |
| Every documentation example compiles                       | On record, automated | `Tests/Unit/PackageExampleTests.cpp`                   |
| Documentation house style with an empty baseline           | On record, automated | `Tests/Unit/PackageDocumentationStyleTests.cpp`        |
| Native behavior executed on all eight cells                | **Not on record**    | The [CI matrix](CI-CD.md), never run at a phase commit |
| Extended CI run pinned to the phase commit                 | **Not on record**    | `CI.yml` with the `extended` scope                     |
| Independent cryptographic review of `Rux/Crypto`           | **Not on record**    | [The review checklist](CryptoReview.md)                |
| `/storage/` documentation routes retired or redirected     | **Not on record**    | The separate website repository                        |

## Evidence Recorded on This Host

Gathered at commit `d0821944` on Windows 11 Pro 26300 x86-64, with `Rux 0.4.0 (2026-11-02 09:09:25 UTC)` built from that tree, after the syntax redesign, with one documentation comment in `Rux/Json` corrected during the run. The numbers below are what the commands reported, not what they were expected to report.

**Three hundred and twelve `rux check` runs, zero failures.** Thirty-nine packages against each of `freebsd-aarch64`, `freebsd-x86_64`, `linux-aarch64`, `linux-x86_64`, `macos-aarch64`, `macos-x86_64`, `windows-aarch64` and `windows-x86_64`. A cell other than the host is compiled and analyzed but never executed, which is exactly the gap the CI matrix exists to close.

**Three hundred and twelve `rux doc` runs, zero failures** once the `Rux/Json` comment that put prose after its tags was fixed; before the fix that package's eight runs failed, which is the kind of mistake the grid exists to catch. Every package test was also checked against `linux-x86_64`, `macos-aarch64` and `freebsd-x86_64`, with no failure.

**Seventy-eight `rux pack` runs and thirty-nine `rux publish --dry-run` runs, zero failures.** Each package was packed twice into separate paths and the two archives compared. All thirty-nine pairs agree byte for byte.

**Thirty-nine distinct documentation routes.** Every manifest's `Homepage` is `https://rux-lang.dev/docs/api/<name>`, all thirty-nine distinct, and none is under the retired `/storage/` prefix.

### Archive Determinism

The digests belong to the tree at `d0821944` and are expected to change whenever a package does. What is being recorded is not the numbers but the property they demonstrate: two packs of one tree produce the same bytes.

| Package     | Bytes   | SHA-256                                                            |
| ----------- | ------- | ------------------------------------------------------------------ |
| Algorithms  | 146,634 | `9efcce2de52b1a20d1f4e5036315382a6eee932754e3816d420ca76f4cb5669b` |
| Allocator   | 111,436 | `51182d6f6cb949f06b9315702df7118b834fb9b842fab40621e247c970930de0` |
| Atomic      | 23,316  | `2c14f3ef66196848551409aec2e502480852407604b37dd0716cdf9741ee8989` |
| C           | 140,859 | `29368d88028659eb4b9fe04a04431323b199e830e86b80a62d6b43f8ce4eda24` |
| Collections | 340,303 | `80787c67697b3684c1d0131053931a214497667b0ef3b06aae4ecf9cc96eff56` |
| CommandLine | 58,274  | `9ac2d59f67e5da06a79951e7f4449286fe124e22f63b3927a02f52c2027afd67` |
| Compress    | 76,431  | `8bc1725b2acd2a355a75ef5239a926f5950943efdcb9d45af5a3f7626fd1b197` |
| Core        | 242,149 | `b5e6c1c07433e72f415888d734a7fcd2401d065c1e805cdd26bcdfd92129fbc8` |
| Crypto      | 106,411 | `16327217a868c59d03adc7e29fce67a61a3b8a42e73c916f22c1b30cfd65c9e1` |
| Encoding    | 82,235  | `d42705380b50890293d3694622c931f90f4b8657bf86521871bc1532663a4bb9` |
| Entropy     | 23,458  | `77c0b63d6b4d80fad6178b75b34143e586e4910be114dfb75800799379267f1c` |
| FileSystem  | 132,849 | `16aea9129f99b3e8e9dbb9310964e126602da4fe0ef83f9984531ccd82d7c6cd` |
| Format      | 359,168 | `924b0a540bcc6cd93f78b3ffb375a3a26c54c5310ac93af17c96f82269284541` |
| FreeBSD     | 108,837 | `c603c945d9615798f60ff0fd6c0836b0ad7f055e02b507a009dd4ab40dd80d61` |
| Hash        | 71,253  | `a89668870b8bbd7cd148ca3cfa0b5186a6e70cb5bd27f0bd61d56bbf1af1c99a` |
| Io          | 144,520 | `e8d0fb972495fe8fcac392e03df62742cd0f70256449913fb6e0c8e3f06c0e91` |
| Json        | 112,185 | `46e8f0815c26c6cb35008c07a761448483aa003f21f8df6e7e09c6f9ae53843c` |
| Linux       | 120,729 | `c6f56584e5dae4363bda28dac2a9c562b8099414b05f1e9eafdf92ddccc6c4bc` |
| Log         | 43,807  | `0f69ebc2d7e8058ee541287218b88f9820244f6a9d2947a3d6118b5c4aac8bd7` |
| macOS       | 104,262 | `9fafec9fb0a11c9755a2dbd95ffe21c73bb248c501b8deffe010c6bd38c0a364` |
| Math        | 185,758 | `294dca5c7ffa98b9f4133519249e205fee1e1eb931ed306b754e21f23d16430e` |
| Memory      | 57,019  | `fcd7645717c9a3d74d90db6c372c06d83889b974cb96062da9edac0acf278a8f` |
| NanoId      | 22,207  | `8a6d792b64754a850261e5a2c8f2c0381a84213268162d1b3172a02a90ccedb2` |
| Net         | 122,213 | `87961710a051d7190deed64cfcf084d22efd75be4b67eb7e4c600e48edb59980` |
| Path        | 70,899  | `f37899e4cf89879c440da1fd8dce07c9258a381b72f2eb86baeb193705ce1a64` |
| Process     | 99,878  | `92478f0813db8c2ecd54a2e21ee0ed17a5b603ca30116392c48b3c7c605de271` |
| Random      | 51,263  | `fbb1b1d0858df6f858be8b12bd3c16b38a29f2576042d973811b7df6603ddfa0` |
| Regex       | 125,910 | `6ea61b72b6c847771908023f6bbd6c594311a99a37f39eb4b58cf0bf944cb3f9` |
| Sync        | 68,114  | `8bf72b95a7d7c946d4f0e9cc9f665a695d0139bb68e9feca230db7109af2ac6f` |
| Test        | 33,633  | `45a22ee48df1d89b5079acf8264f18f0c24bc5dea9da55e16e25efc28dca699c` |
| Text        | 179,985 | `7e445135ef178af808d14f468a4a1f1f59b94c11aa6e473139bafa87d8506087` |
| Thread      | 24,125  | `b464d70474798eb18eddacad2cba6b0b7ebcbc57ea6249c7ab28ae397f86f2d6` |
| Time        | 115,779 | `79c8d19b27ff70a8cddbc5f7649ce3846bf1559ab9e141070b4da254aab36eed` |
| TimeZone    | 41,170  | `c3137ad4dc8c5a8bb585af12c683fac2c6c88973e83f23083475fb45cd651378` |
| Toml        | 163,610 | `59d0d83c4978d9d41abb091d1f6059e8dcac7839e485bf71960a3fbc9230542d` |
| Unicode     | 719,261 | `7930a9a39673e0c8e7f32abda085ee4357a532dcbdb69edbd849bd4e0c403dbd` |
| Url         | 50,672  | `58ab31fcc17a00470d147fdbe6100ad31493a1a2808c77ad2ffa0b1db7459ca9` |
| Uuid        | 37,252  | `9027fd9182094e210799b2468ce639d317ccefbf8abb9aa5e1f012e8d0d1bac9` |
| Windows     | 187,008 | `898846300efa1cbd4031644678ec2bd5d275ad466bafac57b501d60673f6f580` |

### Documentation

The house style is a rule rather than a ratchet: `Tests/Unit/PackageDocumentationStyleBaseline.txt` holds a comment header and nothing else, so a new violation fails outright. It once held 4,782 entries.

Every fenced `rux` block in every package README is compiled by `Tests/Unit/PackageExampleTests.cpp`: forty compile, and five are fragments that lean on names their surrounding prose supplies. That test exists because five examples were broken before anything looked at them, and no other check would have found any of them.

## What the Test Suite Proves on This Host

Five hundred and thirty-two package and language tests pass, and eighteen CTest entries pass. Read that number carefully, because it does not mean what it appears to mean for the three POSIX platform packages.

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
sh Run.sh sync
```

On Windows, use `./Run.ps1 sync`.

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
