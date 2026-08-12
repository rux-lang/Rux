# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Each release groups its entries under **Added**, **Changed**, **Removed**, and **Fixed**, subdivided by area (Language, CLI / Package Manager, Tooling, Platform, Runtime / Linker) when a section is long enough to need it.

## [Unreleased]

Introduces compile-time programming (`when`, `intrinsic`, `#`-prefixed compiler context), native AArch64 host support on every platform, an explicit `let`/`var` mutability model, and a consolidated, hermetic test tree.

### Added

#### Language

- **`when` — conditional compilation** — `when cond { ... } else when cond { ... } else { ... }` evaluates its condition while compiling and keeps only the taken branch; untaken branches are never resolved or type-checked, so they may reference symbols that do not exist on the current build. `if` remains the run-time conditional, and a chain keeps the keyword it opened with — `else if` inside a `when` chain is an error. Conditions are built from `const` declarations, enums, literals, imported compile-time intrinsics, and the usual operators. `when` works between declarations and inside a function body, where it introduces no scope of its own. A condition that is not a compile-time constant, or not a `bool`, is an error.
- **`when` match over a compile-time value** — `when <subject> { pattern => body, ... else => body }` keeps only the matching arm: `when #target.os { .Windows => PrintLine("win"), .Linux => PrintLine("linux"), else => #Error("Unsupported OS") }`. Arm patterns are enum variants or literals, an arm may list several comma-separated patterns, and `else` is the default. An arm body is an expression, a `{ ... }` block, or — between declarations — a declaration or an `#Error`/`#Warn` directive. If no arm matches and there is no `else`, the build is an error.
- **`intrinsic`** — marks a declaration the compiler supplies rather than Rux source: `intrinsic #target: Target;` and `intrinsic func Assert(condition: bool, message: char8[]);`. A `#`-prefixed name marks a compile-time value or function (`#target`, `#Error`); ordinary runtime intrinsics (`Assert`, `Panic`) and methods keep their plain names. An intrinsic value has no initializer and an intrinsic function has no body. Replaces the `#Intrinsic("Name")` attribute and the `$` sigil.
- **Compile-time build context** — importing the Core package exposes ordinary values for the target (`#target.os`, `.arch`, `.abi`, `.endian`, `.pointerBits`, `.dataModel`, `.objectFormat`, `.triple`, `.HasFeature(...)`), build (`#build.profile`, `.mode`, `.optimization`, `.debugAssertions`, `.debugInfo`, `.isTest`, `.outputKind`, `.timestamp`, `.date`, `.time`), compiler (`#compiler.version`, `.HasFeature(...)`), and source location (`#source.line`, `.column`, `.file`, `.fileName`, `.filePath`, `.function`, `.module`). `#config.Get("name")` and `#config.Has("name")` read user-defined build values.
- **`#target.os` and the `OperatingSystem` enum** — `#target.os` is a compile-time value of the built-in `OperatingSystem` enum, so a build selects platform code with `when #target.os == .Windows { ... }`. Each variant is a system rather than a family — `Windows`, `Linux`, `MacOS`, `FreeBSD`, `OpenBSD`, `NetBSD`, `DragonFlyBSD`, `Illumos`, `Solaris`, `Android`, `IOS`, `AIX`, `Haiku`, `Fuchsia`, `QNX`, `Redox` — so the BSDs are told apart. A variant may be written as `Enum::Variant` or as the `.Variant` shorthand when the other side fixes the enum. A variant the enum does not have is an error; one no build target can currently produce is a warning. A condition may only name an intrinsic or built-in enum the file has brought into scope.
- **`#Error` and `#Warn` compile-time directives** — `#Error("message")` and `#Warn("message")` emit a diagnostic at the call site and produce no runtime code, so a build can reject an unsupported configuration: `when #target.os == .Windows { #Error("Windows is not supported yet"); }`. They run after `when` folds, so a directive in an untaken branch never fires. The message must be a string literal.
- **Intrinsic assertions** — importing `Assert` or `DebugAssert` from the Core package enables compiler-backed assertions that report the message, function, file, line, and column before trapping. Release builds remove `DebugAssert` checks without evaluating their arguments.
- **Intrinsic panic and no-return functions** — importing `Panic` from the Core package enables compiler-backed panic reporting and termination. `#NoReturn()` marks user or extern functions that never return and makes calls terminate their control-flow path without adding a new type or keyword.
- **`byte` primitive alias** — `byte` is the raw-storage spelling of `uint8`, with identical representation, ABI, constants, conversions, and overload identity. Memory operations use it for byte-addressable views while numeric code can retain the explicit `uint8` spelling.
- **Primitive associated constants** — Boolean, character, integer, and floating-point types expose their storage width through `Bits` and `Bytes`. Integers and characters also expose `Min` and `Max`; floating-point types expose `Lowest`, `Max`, `MinPositive`, `Epsilon`, `Infinity`, and `NaN`. The `bool`, `char`, and `float` aliases forward to their canonical widths, while `int` and `uint` follow the compilation target.
- **Future primitive type reservation** — documented primitive names whose representations are not implemented yet, such as `int128`, `float16`, `bool64`, and `char128`, are reserved and produce a compiler-version diagnostic instead of being reported as unknown types.
- **Logical right shift** — signed integers support `>>>` and `>>>=` to shift in zero bits while preserving the left operand's type and fixed width; `>>` remains arithmetic for signed integers and logical for unsigned integers.
- **Whole-module import** — `import Pkg;` binds a dependency's eponymous `module Pkg` as a namespace, so its members are used through `Pkg::Name` (e.g. `import Windows;` then `Windows::HeapAlloc(...)`). A package that exposes no same-named module still requires naming an item, and `import Pkg::Module` / `import Pkg::{ A, B }` / `import Pkg::*` are unchanged.
- **Inline assembly functions** — `asm func Name(...) -> T { ... }` bodies are written directly in x86-64 (Intel syntax) and assembled to machine code, bypassing the normal HIR/LIR pipeline. Supports the common instruction subset (ALU ops, `mov`/`lea`, `movzx`/`movsx`, the multiply/divide group, shifts, `push`/`pop`, `call`/`jmp`, the full `jcc`/`setcc` family, `ret`/`leave`/`nop`/`syscall`), register/immediate/memory operands, local labels, and calls to other functions. A body is read and assembled for the architecture the build targets: an AArch64 build accepts AArch64 register names and operand syntax (`#imm`, `[Xn, #off]`, `[Xn, Xm, LSL #3]`, `[Xn], #off`, `[Xn, #off]!`, `X1, LSL #3`, `B.EQ`) and encodes the body itself, and a body whose instructions belong to the other architecture — an `imul` in an AArch64 build, an `ldr` in an x86-64 one — is an error naming the mnemonic that gives it away. One function reaches both machines by writing a body per architecture under `when #target.arch`.
- **Targeted lint allowances** — `#Allow("naming.type")` preserves intentional foreign type, field, and variant spellings on a single type declaration without disabling naming checks elsewhere.

#### CLI / Package Manager

- **`rux doc`** — generates deterministic, self-contained HTML API documentation after the normal compiler frontend and semantic analysis succeed. Outer `///` comments support a safe Markdown subset, target and define selection match `rux check`, public API is the default, `--document-private-items` includes private declarations, and managed-directory markers prevent `--output` from replacing unrelated files.
- **Versioned CLI contract** — `rux help --json` and `rux help <command> --json` publish schema version 1 for documentation tooling, with the program version, global options, command usage, arguments, options, examples, and stable documentation URLs.
- **Cross-target builds** — a `--target <triple>` naming something other than the host is no longer refused, and `rux run` and `rux test` accept the option that `build`, `check`, `doc`, `install`, and `update` already had. x86-64 machine code and executables are produced entirely in-process, so a Linux host writes a Windows PE or a macOS Mach-O with no external toolchain and no change in output for the host triple. AArch64 output still lowers through the platform Clang driver, so it remains limited to a matching host; asking for it elsewhere names the architecture that has no back end yet instead of quietly building for the host. `run` and `test` execute what they build, so they additionally require a target this host can run — directly, or under an emulator, described in the next entry.
- **Emulated `rux run` and `rux test`** — an artifact built for another architecture is executed through a user-mode emulator instead of being refused, so a cross build is testable without a second machine. The emulator is the one named by `RUX_EMULATOR`, or the architecture's usual QEMU binary (`qemu-aarch64` for AArch64); `RUX_QEMU_SYSROOT` is passed on as `-L`, which is where a dynamically linked guest program finds its loader and shared libraries. Exit codes and output pass through unchanged, so the `rux test` contract — a test passes by exiting `0` — reads the same for every target. An emulator that is not installed is reported by name, along with the package that supplies it, rather than surfacing as a failed exec. A foreign *operating system* is still refused: an emulator supplies an instruction set, not a kernel. `Test.sh --target <triple>` and `Test.ps1 -Target <triple>` run the whole Rux suite this way, leaving the C++ unit tests, formatting and static analysis on the host.
- **Per-target output directories** — a build for a target other than the host writes to `<Output>/<Profile>/<triple>/`, so building the same package for several targets no longer has each one overwrite the last. The host target keeps the `<Output>/<Profile>/` path it has always had, and a host build is byte-identical whether the triple is named explicitly or left out. Artifact names follow the target operating system rather than the host, so a Linux host produces `Name.exe` for `windows-x86_64` and `libName.dylib` for a macOS shared library. `rux build --stats` names the target it built for, and the one-line summary mentions it whenever it is not the host.
- **`rux publish` and `rux pack`** — publication transport, the write half of the registry contract. `rux pack` builds the package archive, a ZIP named `<Name>-<Version>.ruxpkg` holding `Rux.toml` at its root, every file below `Src/`, and the files named by `ReadmeFile` and `LicenseFile`; entries are sorted and carry a fixed timestamp, so packing one tree twice produces identical bytes. `rux publish` uploads that archive together with the exact manifest bytes it embeds. The bearer credential comes from `RUX_TOKEN` or from `rux login`, and never from a flag, keeping it out of shell history and the process list; `RUX_REGISTRY_URL` or `--registry <url>` selects the registry, and `--dry-run` validates and packs without uploading. The registry's RFC 9457 problem responses are reported as ordinary CLI errors, so an immutable-version conflict, an unclaimed namespace, or a token missing the `publish` scope each explain themselves. The matching read half is described under **Changed**: `rux install` now resolves and downloads through the same registry.
- **`rux login` and `rux logout`** — store a registry credential once instead of exporting `RUX_TOKEN` for every publish. The token is read from stdin, never from an option, so it stays out of shell history and the process list; a terminal is prompted without echo, and any other stdin is read as one line, so `echo "$TOKEN" | rux login` works. Tokens are stored beside the package cache in `%LOCALAPPDATA%\Rux\Credentials.toml` (`$HOME/.rux/credentials.toml` elsewhere), restricted to the owner and written through a temporary that is locked down before the token reaches it. Entries are keyed by registry base URL rather than kept as one ambient token, so retargeting `--registry` at a local registry cannot leak the credential for the official one, and `rux logout` removes just the selected registry's entry. `rux login` verifies the token against the registry first and refuses one that is rejected; a registry that cannot be reached or does not implement the check warns and stores it unverified. `RUX_TOKEN` still takes precedence over a stored token, so CI is never shadowed by a file left on a self-hosted runner, and `rux publish` now names whichever source actually supplied the credential when the registry rejects it.
- **Manifest publication validation profile** — the profile documented for Version 1 is now enforced. `rux publish` and `rux pack` reject a workspace manifest, a missing `Namespace` or `MinRux`, a `MinRux` below `0.4.0`, and every path dependency before any archive is built or any request is sent; every other command keeps the permissive local profile. First-party packages now declare `MinRux = "0.4.0"`, and the repository manifest-policy test holds them to the publication profile.
- **Compile-time configuration** — `[Build.Defines]` supplies string values to `config`, and `--define NAME[=VALUE]` overrides them for `build`, `check`, `run`, and `test`. Date/time compiler parameters share one UTC build timestamp and honor `SOURCE_DATE_EPOCH` for reproducible builds.
- **Manifest-less workspace installation** — bare `rux install` now discovers package manifests below the root `Tests/` tree, immediate member packages, and member `Tests/` trees when no root `Rux.toml` exists. Registry dependencies are deduplicated and installed through the existing transitive resolver, so repository test setup no longer needs a hard-coded package list.
- **`rux uninstall --global`** — remove every package from the global cache, whether or not the current `Rux.toml` declares it. Completes the `--global` set alongside `rux list --global` and `rux update --global`.

#### Tooling

- **AArch64 host support** — FreeBSD, Linux, macOS, and Windows now build and run the compiler natively on AArch64, with architecture-correct target selection, platform-aware lowering, and full CI build/test coverage alongside x86-64.
- **C++ static analysis** — `.clang-tidy` defines an enforced, high-signal Clang 22 baseline for compiler and unit-test translation units. The portable test workflows run the analysis in parallel from CMake's compilation database when passed `-ClangTidy` or `--clang-tidy`, and the lint CI workflow always rejects new findings.
- **Portable repository scripts** — `Build.sh` and `Test.sh` provide the same build and complete verification entry points as their PowerShell counterparts on Linux, macOS, and FreeBSD.
- **Source formatting scripts** — `Format.ps1` and `Format.sh` format or check all maintained C++ and Rux sources while excluding vendored code and malformed diagnostic fixtures.

### Changed

#### Language

- **Explicit binding and pointee mutability** — `let` bindings and parameters are immutable by default, while `var` declares a mutable binding or parameter. Pointer pointee mutability is written `*T` (read-only) or `*var T` (writable), and `@place` infers the pointer form from the addressed place. This replaces `let mut`, `*mut T`, and `@mut`.
- **Fully-qualified item imports** — importing an item now requires its complete `Package::Module::…::Item` path; the shortcut that let a bare `import Package::Item` reach into a package's same-named module is gone. `import Memory::Alloc` is an error (with a `did you mean 'import Memory::Memory::Alloc'?` hint) — the package name is always the first segment and the containing module must be named. Items declared at a package's root (`import Core::{ #target }`) are unaffected.
- **Extern import syntax** — `#Link("Kernel32.dll")` imports an extern function under its Rux declaration name, while an optional second argument such as `#Link("Kernel32.dll", "Beep")` names a different exported DLL symbol. The one-argument form also applies a library to an extern block. `#Library` and `#Symbol` remain compatibility spellings but cannot be combined with `#Link`.
- **`#`-prefixed compiler context and PascalCase target variants** — compiler-provided values are now `#target`, `#build`, `#compiler`, `#config`, and `#source`. Target enum variants use unambiguous spellings such as `IOS`, `X86_64`, and `RiscvLp64`; Windows ISO code pages use names such as `Iso8859Part1`.
- **Native Linux calling convention** — ordinary and inline-assembly Rux functions now use the System V AMD64 ABI on Linux, including `rdi`/`rsi`/`rdx`/`rcx`/`r8`/`r9` integer arguments, 16-byte call-site stack alignment, and stack-passed overflow arguments. `#Abi(.Win64)` remains available as an explicit override.

#### CLI / Package Manager

- **0.4 CLI migration** — this is a pre-release contract break with no deprecated aliases:

  | Before                                                                                                     | 0.4.0                                              | Migration                                                         |
  | ---------------------------------------------------------------------------------------------------------- | -------------------------------------------------- | ----------------------------------------------------------------- |
  | `--color on\|off\|auto`                                                                                    | `--color always\|never\|auto`                      | Rename explicit color values.                                     |
  | `build --dump-tokens`, `--dump-ast`, `--dump-sema`, `--dump-hir`, `--dump-lir`, `--dump-asm`, `--dump-rcu` | `build --emit <kind[,kind...]>`                    | Pass one repeatable option, for example `--emit ast,sema`.        |
  | `build --profile <name>`                                                                                   | Removed                                            | Use the explicit `--debug` or `--release` profiles.               |
  | `new` / `init` `--bin`, `--lib`                                                                            | `--executable`, `--shared`, `--static`, `--source` | Executable remains the default.                                   |
  | `fmt` formatted source only                                                                                | `fmt` formats source and `Rux.toml`                | Use `--source-only` or `--manifest-only` to narrow the operation. |
  | Command parsing returned mixed failure codes                                                               | Invalid usage returns 2                            | Operational and compiler failures continue to return 1.           |

- **Manifest Version 1** — `Rux.toml` is now a versioned, strictly parsed contract shared with the package registry, and the cutover is hard: every manifest opens with a `[Manifest]` table declaring `Version = 1`, and unversioned or legacy files are errors rather than being silently upgraded. `Type` is exactly `Executable`, `SharedLibrary`, `StaticLibrary`, or `SourceLibrary`; the retired `Program`, `Library`, and `Source` values are errors. `[Package]` gains `Namespace`, `Keywords`, `LicenseFile` and `ReadmeFile`, and `[Package].Version` must be a strict Semantic Version. `License` names the terms as an SPDX expression and `LicenseFile` carries their text inside the package; they are independent, so a package may declare either, both, or neither. `Repository` and `Homepage` must be absolute `http`/`https` URLs that carry a host and no credentials. Dependencies are inline tables — `Io = { Namespace = "Rux", Version = "^1.0.0" }` for a registry package and `Util = { Path = "../Util" }` for a local one — with an optional `Package` field when the import name differs from the package name; import names that collide after `_`-to-`-` lowercase normalization are rejected. Version requirements accept `*`, caret, tilde, comparator intersections and partial operands, and an operand with no operator is a caret requirement, so `1.2.3` means `^1.2.3` and `=1.2.3` pins. Parsing rejects malformed TOML, duplicate keys and sections, unknown sections and fields, wrong value types, and missing required fields, and every failure reports the manifest path, line and column instead of a bare "failed to parse". See the [manifest contract](Docs/Manifest.md).
- **The intrinsics package is `Rux/Core`** — the package supplying the compile-time context, the diagnostic directives, `Assert`/`Panic`, and the primitive and range types is named `Core` rather than `Rux`, so `rux add Rux/Core` reads as an identity instead of repeating itself, and the namespace introduced with Manifest Version 1 no longer stutters in every dependency table, cache path and registry route. Dependents bind it as `Core = { Namespace = "Rux", Version = "..." }` and write `import Core::{ #target, Assert }`; the name is a manifest's choice, so `Rux = { Namespace = "Rux", Package = "Core" }` keeps the old spelling for anyone who prefers it. Nothing had been published yet, so no released identity changes.
- **First-party packages publish under the `Rux` namespace** — every package under `Packages/` now declares `Namespace = "Rux"`, and they depend on each other through the registry form: `Memory = { Namespace = "Rux", Version = "*" }`. Repository test packages stay namespace-free on purpose, since they are built in place and never published, and continue to use `{ Path = "..." }` entries into `Packages/`. A repository-wide manifest-policy test enforces both halves along with the schema header, package type, dependency form, centralized test outputs and canonical formatting, so `rux fmt --manifest-only` is a no-op on every checked-in manifest.
- **Package kinds drive command behavior** — `Executable` links and runs a program; `SharedLibrary` emits `.dll` plus its Windows import `.lib`, `.so`, or `.dylib`; `StaticLibrary` emits `.lib` or `.a`; and `SourceLibrary` is compiled into dependents but cannot be built or run directly. `rux new` and `rux init` select the kind with mutually exclusive `--executable`, `--shared`, `--static`, and `--source` flags. Rux 0.4.0 permits `rux pack` and `rux publish` only for `SourceLibrary` packages.
- **Workspace test discovery** — `rux test` at a workspace root now discovers each member package's `Tests/` directory in addition to the root `Tests/`, so test packages can live beside the code they cover. Tests found under a member are labeled with the member's name (`Text/Tests/Compare` reports as `Text/Compare`), and a root `Tests/` keeps working as before.
- **Flat test executable output** — `rux test` writes each executable directly to its configured `[Build].Output` directory instead of adding a `Debug` or `Release` subdirectory. Ordinary `rux build` and `rux run` outputs remain profile-specific.
- **`rux install` resolves through the package registry** — the read half of the registry contract now ships, so installing a package no longer downloads source from GitHub. `rux install`, `rux update`, `rux add` and `rux info` read the registry's resolver index, and `--registry <url>` or `RUX_REGISTRY_URL` retargets them exactly as it retargets `rux publish`; the read routes are public, so no credential is involved. Resolution walks the whole dependency graph before anything is downloaded: a version is eligible when it is not yanked, its `MinRux` is no newer than the running compiler, and it satisfies every requirement gathered for that package, and the highest eligible version wins. One version is selected per package, and requirements that cannot hold together are reported rather than resolved arbitrarily. Each selection is downloaded as the published `.ruxpkg`, checked against the SHA-256 the registry publishes for it, and unpacked under the same archive contract `rux pack` applies, so an entry path cannot escape the package and a corrupted or substituted archive installs nothing. `[Dependencies]` version requirements are therefore honored for the first time — `Version = "^0.1.0"` used to be decorative.
- **Version-keyed package cache** — installed packages live at `<cache>/<namespace>/<name>/<version>`, using the spelling the package was published under for the namespace and name directories and the exact version text for the leaf, so several versions of one package can be installed at once and two projects no longer contend over one directory. An existing directory is matched by normalized name, so a manifest that spells a dependency `Namespace = "rux"` still resolves to the `Rux` the registry published rather than downloading a second copy beside it. `rux build`, `rux check`, `rux run` and `rux test` pick the highest installed version matching the manifest requirement, entirely from disk, so a build still needs no network; a requirement nothing installed satisfies now names the versions that are installed. `rux uninstall Namespace/Name` removes every installed version, and an added `@requirement` narrows it. `rux list --global` reports each version, and `rux list` names the version a build would actually use. A cache written by an earlier release is deleted on the next install and reinstalled under qualified identity.
- **Registry-shaped JSON and artifact reading** — the compiler gained a real JSON reader for the nested documents the registry answers with, a self-contained SHA-256, and a `.ruxpkg` extractor that verifies every entry's CRC-32 and holds a downloaded archive to the Artifact v1 limits. No third-party dependency was added.

#### Tooling

- **Hardened CI and releases** — all workflows use current action versions, least-privilege permissions, credential-free checkouts, bounded timeouts, consistent `x86_64` artifact names, and seven-day intermediate artifact retention. Every platform now runs C++ unit tests plus Rux check, lint, and test verification; release assets include SHA-256 checksums.
- **CMake 3.30 baseline** — Rux now accepts CMake 3.30 or newer while retaining policy behavior through CMake 4.3. FreeBSD CI uses the packaged CMake 3.31 release instead of spending more than 20 minutes building CMake from source.
- **Canonical architecture names** — documentation and CI labels use `x86-64` and `AArch64`; machine-readable target triples and artifacts use `x86_64` and `aarch64`; C++ and Rux architecture enums use `X86_64` and `AArch64`. The CLI still accepts `x64`, `amd64`, and `arm64` target suffixes as compatibility aliases and normalizes them before compilation.
- **Architecture-aware object and linker layer** — an RCU object records the machine it was compiled for as a named architecture rather than a fixed byte, and the relocation table gained the AArch64 kinds — `CALL26`, `JUMP26`, `CONDBR19`, `TSTBR14`, `ADR_PREL_PG_HI21`, `ADD_ABS_LO12_NC`, `LDST_ABS_LO12_NC`, `MOVW_UABS_G0` through `G3`, `PREL32`, and `PREL64` — whose split immediates a whole-field patch cannot express. The linker, the relocatable-object writer, and the archive writer take the target architecture alongside the target operating system, so the machine identifier stamped into an ELF, COFF, or Mach-O object follows the target instead of being fixed to x86-64. An object compiled for a different architecture than the link target, a relocation no object writer can encode yet, and an executable format that has no back end for the target architecture are each reported as an error rather than producing a silently corrupt artifact. `rux build --emit rcu` names the object's architecture and prints the new relocation names.
- **Hermetic consolidated tests** — repository tests are organized as language, package, unit/golden, and policy suites below `Tests/`. Every Rux test dependency is an explicit local path, while workspace-member overrides resolve transitive first-party dependencies without registry access.

#### Platform

- **AArch64 system-call wrappers** — `Syscall0` through `Syscall6` in the Linux, macOS, and BSD packages now have real AArch64 bodies selected by `when #target.arch`, alongside the x86-64 ones they already had: the call number moves into `x8` (`x16` on Darwin), the arguments shift down one register into `x0`-`x5`, and `svc #0` (`svc #0x80` on Darwin) traps. macOS and BSD normalize the carry-flag error convention to the `-errno` the Linux wrappers return. Darwin's syscall class, which only the x86-64 trap carries, is zero on AArch64 so the ordinals are passed bare. `Math`'s `Sqrt`, `FloatBits`, and `FromBits` likewise gained AArch64 bodies. The x86-64 build of every package is byte-for-byte unchanged.

### Removed

- **Metadata blocks** — every `#{...}` declaration metadata block is now an error. Calling conventions use `#Abi(.C)`, `#Abi(.SysV)`, or `#Abi(.Win64)`; platform selection uses conditional compilation such as `when #target.os == .Windows { ... }`.
- **Compiler-parameter sigils** — the `$`-sigil compiler parameters, their flat aliases, and the `#Intrinsic("Name")` attribute are gone. Compile-time values are `#`-prefixed and declared with the `intrinsic` keyword.
- **Target-specific dependency sections** — `Rux.toml` no longer recognizes `[Dependencies.Target.<OS>]` (or the legacy `[Target.<OS>.Dependencies]`); only a single `[Dependencies]` table is allowed. Platform selection belongs in source with conditional compilation.
- **GitHub downloads and the `git` runtime dependency** — packages are no longer fetched from repositories. The flat name-to-repository index, the GitHub tree and `raw.githubusercontent` requests, and the `git clone` that backed them on Windows and was the only implementation on Unix are all gone, so `git` is no longer needed to use the toolchain — only to build it from source. Installing a package by bare name went with them: a registry package is identified by `Namespace/Name`, as `rux add` already required.
- **`rux install --dev`** — the flag existed only to clone a repository's `dev` branch, which has no meaning now that installs come from published, immutable versions. Use a version requirement, or a `{ Path = "..." }` dependency for unpublished work.

### Fixed

- **`when` conditions under a renamed intrinsics dependency** — a `when` condition only recognized a build intrinsic or built-in enum when the intrinsics package happened to be imported under the literal name `Rux`, because the fold matched the import spelling rather than the package it resolved to. `Package` exists so a manifest can bind a dependency to any import name, and doing so silently cost conditional compilation: identical source compiled under `Rux = { ... }` and failed with `unknown identifier '#target'` under `Core = { ... }`, while ordinary calls to the same intrinsics kept working. The driver now resolves which import names refer to the package by identity — reading the target manifest for a path entry, which is how repository tests reach it — and the fold matches those. Any alias works, including keeping `Rux`.
- **Constants of slice type** — `const Name: T[N] = [...]` and `const Name: char8[] = "..."` are now emitted into read-only data with a `{data, length}` header, instead of an eight-byte placeholder that was never filled in. Reading, indexing, iterating, or taking the `.length` of such a constant crashed at runtime. Elements may be literals, negated literals, or other named constants; anything the backend cannot lay out is now a compile error rather than a corrupt read.
- **Module scope in call resolution** — a call now binds to a function from its own module or from a module the file imports, in preference to a same-named function declared elsewhere in the program. Functions in different modules are no longer treated as overloads of one another, and two modules that would emit the same symbol are disambiguated by their module path, so importing one package could silently redirect a call into another.
- **Duplicate function signatures** — declaring the same function or method signature twice now reports the second declaration as an error. Functions with distinct parameter types remain valid overloads, and same-named functions in separate modules remain independent.
- **Dependency compilation** — `rux check` and `rux build` now share the same dependency-loading pipeline, so installed and path-based dependencies are available during semantic analysis and unresolved dependencies stop before code generation.
- **Call-site diagnostics on extern functions** — `#Error("...")` and `#Warn("...")` now emit their diagnostics when an extern function is called, including through module-qualified names, instead of being discarded after parsing.
- **Attributes on extend methods** — attribute calls such as `#Abi(...)` are accepted on methods inside an `extend` block instead of failing with "expected 'func' in extend body" (#197).
- **Zero-operand assembly parsing** — instructions such as `syscall` followed by `ret` are now parsed as two instructions instead of treating the second mnemonic as an operand.
- **AArch64 conditional branches inside a foreign-target `when` arm** — an `asm func` written under `when #target.arch { .AArch64 => ... }` failed to parse an x86-64 build with `expected an assembly mnemonic, found '.'` as soon as its body used `b.eq` or any other condition-carrying branch, because the dot joining a mnemonic to its condition was only read when the build already targeted AArch64. An untaken arm has to parse before it can be discarded, which is why `#` immediates were already read everywhere; the branch form now is too. A body that really is compiled for the wrong architecture still names the mnemonic that gives it away.
- **Methods of an `extend` on a concrete generic type** — `extend Slice<int> { ... }` and any other `extend` naming a generic type's arguments in full emitted each of its methods twice under one symbol: once with the block, and once more as a monomorphization requested by the first call site, whose receiver was a pointer where the block's was the value. Only a block already naming a type parameter of its own — `extend List<T>` — is monomorphized now.
- **`String` as a format argument** — `Format`, `Print`, and `PrintLine` rejected a `Text::String` argument with `cannot pass 'String' to variadic parameter of type 'Stringable'`. `Rux/Format` extended every primitive and `Slice<char8>` with `Stringable` but never `String` itself, so the one type the protocol hands back was the one type it would not accept, and `PrintLine("{}", text)` had to be written around. `String` now implements `Stringable` and renders as a copy of its own bytes — a copy rather than the receiver's block, because `Format` frees the String each argument renders itself into.
- **`DT_RELAENT` in dynamically linked ELF images** — the entry size was written only when the image also carried a `.rela.dyn`, so an ordinary dynamic executable declared `DT_JMPREL` and `DT_PLTREL = DT_RELA` with no entry size to read them by, and `readelf --use-dynamic --relocations` reported a corrupt dynamic tag instead of the PLT relocations. `DT_RELAENT` covers `.rela.plt` as well and is now always present.
- **Relocatable ELF object conformance** — a `StaticLibrary` package's `.o` members described a section at index 0, where an index of 0 means "none": the reserved null section header was written as `PROGBITS` with an alignment of 1 rather than all zeros, so `readelf -S` listed a phantom section. Their `.rela` sections also carried no flags, leaving `SHF_INFO_LINK` unset — the flag that says `sh_info` names the section the entries patch rather than being a count, as it is in a symbol table. Both are now written the way an assembler writes them.
- **Windows MSI license packaging** — the installer now packages the canonical `LICENSE.md` file and displays the same copyright notice as the repository license.
- **Repository integration consistency** — FreeBSD package metadata now declares its `Rux` dependency, macOS scripts locate Homebrew's versioned LLVM 22 tools, CI enforces Rux formatting, Nix runs CTest from the correct build directory, and documentation consistently uses the `Build/` directory and centralized test workflow.

## [0.3.0] - 2026-06-23

Adds broad multi-platform host support, a revamped platform abstraction layer, new language features, a macOS linker backend, and Windows DLL output, along with correctness fixes, broader literal and constant-expression support, improved overload resolution, expanded runtime support, better test coverage, cleaner CLI/build tooling, and many bug fixes.

### Added

#### Language

- **Target attributes** — `@[Target(...)]` attributes to conditionally compile code per platform
- **Unicode escape sequences** — `\u{...}` escapes in string and character literals
- **Constant integer expression coercion** — compile-time folded integer expressions now coerce to sized integer targets when the value fits
- **Typed non-decimal integer suffixes** — `0xFFu`, `0b1010u`, `0o17i`, and underscore separators in non-decimal literals
- **Constant character cast validation** — compile-time validation for `as char8`, `as char16`, and `as char32`
- **Boolean bitwise operators** — `&`, `|`, `^`, and `~` on `bool` types
- **Attribute handling improvements** — `@[Target(...)]` import filtering by platform, plus warning and error attribute support (`@[Warn(...)]`, `@[Error(...)]`)

#### Runtime / Linker

- **Windows PE32+ DLL output** — emit `.dll` artifacts when `Type = "Dll"` in `Rux.toml` (export directory, optional `DllMain`)
- **macOS Mach-O linker backend** — native x86-64 Mach-O executable output on macOS
- **macOS `munmap` thunk** — adds `munmap` support to the Mach-O linker so `Std::Memory::Free` can release mmap-backed allocations
- **Floating-point remainder support** — adds FP `%` handling
- **Floating-point comparison fixes** — correct FP comparison behavior

#### Platform

- **OpenBSD x86-64 host** — native compilation and execution on OpenBSD x86-64
- **NetBSD x86-64 host** — native compilation and execution on NetBSD x86-64
- **DragonFly BSD x86-64 host** — native compilation and execution on DragonFly BSD x86-64
- **Platform abstraction layer** — revamped `Platform` implementation with platform macros and CPU feature detection at runtime
- **BSD ELF target support** — correct ELF OSABI, `PT_NOTE`, `ET_DYN` per target
- **Target-specific platform dependencies** — `[Target.<Platform>.Dependencies]` in `Rux.toml`
- **Syscall thunks** — `nanosleep` and `clock_gettime` typed thunks (Linux); `RtlCompareMemory` thunk (Windows)

#### CLI / Package Manager

- `rux check` — type-check the current package without producing a binary
- `rux info [--json]` — display installed package information; `--json` outputs machine-readable JSON
- `rux install --dev` — install a package as a dev dependency
- Inline TOML table dependency fields in `Rux.toml`

### Fixed

#### Language

- **Overload resolution** now hard-errors on unresolved overloads instead of silently falling back
- **Bare integer literals** now work in single-overload resolution
- **Binary expression operands** are now type-checked on both sides
- **`is` folding** now produces a compile-time boolean instead of emitting a fake call
- **Tuple size / field offset layout** is now aligned consistently across backends
- **Pointer arithmetic** now scales by element size
- **Slice-of-slice assignment** no longer corrupts the slice length field
- **Compatibility checks** for mixed character and integer types are relaxed where appropriate
- **Platform-conditional imports** under `@[Target(...)]` are filtered correctly during dependency collection
- **Constant casts** now reject out-of-range character values and Unicode surrogate code points
- **Out-of-range integer literals** now produce clearer diagnostics outside `let` bindings
- **Unsigned/sized integer literal handling** is now consistent across decimal, hex, binary, and octal forms
- Enums and type aliases not resolving inside `extern` blocks
- `bool` and `float` type handling regressions
- Double pointer parsing bug

#### Runtime / Linker

- **Integer `**` (power) operator** — defines the `__rux_ipow` runtime helper
- **SysV stack argument passing** is restored in codegen
- **Entry stack alignment** — pre-adjust RSP before `call Main`
- **`ReadFile`/`WriteFile` thunks** — preserve R9 across syscall, guard `mov [r9]` with a null check, preserve non-volatile RDI/RSI on Win64

#### Platform / Tooling

- **Windows CMake linking** issues are fixed
- **GCC/MinGW** terminal-link workaround is added
- **Install/uninstall** now work outside project directories
- **Help command handling** is cleaner, more robust, and terminal-width aware
- **Type dependency resolution** and wildcard target handling are fixed
- Platform dependency resolution via wildcard targets and robust TOML parsing
- OpenBSD ELF header fixes for `execve` compatibility
- OpenBSD test cases are removed where they no longer apply
- Windows `std::max` macro conflict with compiler internals
- UB in `gitclone` due to a missing `return`

## 0.2.2 - 2026-05-28

Expands the package manager CLI, adds Linux and FreeBSD host support, and fixes several compiler bugs.

### Added

#### CLI / Package Manager

- `rux install [package][@version]` — install a package into the current project
- `rux uninstall [package]` — remove a package from the current project
- `rux list [--global]` — list installed packages
- `rux update [--global]` — update packages to their latest versions
- `rux add --path <path>` — add a local package dependency by path

#### Platform

- **Linux x86-64 host** — native compilation and execution on Linux x86-64
- **FreeBSD x86-64 host** — native compilation and execution on FreeBSD x86-64
- Linux syscall thunks for I/O (`ReadFile`, stdin support via `GetStdHandle`)

### Fixed

- Parsing bugs with the `as` keyword
- Compiler bugs with `const` declarations, `import` statements, and calling conventions
- Incorrect handling of integer literals with suffixes (`10i`, `10u`) and range expressions (`0..10u`)
- `rux add` crash when specifying an unknown package name

## 0.2.0 - 2026-05-10

Expands the compiler with control flow, composite types, modules, and a richer type system.

### Added

#### Language

- **Control flow** — `if`, `for`, `while`, `do-while` statements
- **`sizeof` operator** — returns the byte size of a type
- **Slices** — variable-length views over contiguous memory
- **Tuples** — fixed-size anonymous product types
- **Enums** — named sum types
- **Interfaces** — structural contracts for types
- **`extend` blocks** — method implementations for types
- **`module` keyword** — declares the module a source file belongs to
- **Function overloading** — multiple functions with the same name and different signatures
- **Function imports** — call functions from other modules
- **Packages** — multi-file compilation units with dependency resolution

#### CLI / Package Manager

- `rux build` now prints build statistics (files, lines, time) after a successful build

### Fixed

- Dependency resolution error when packages referenced each other
- Incorrect code generation for `if` conditions
- Type checking regressions in slices, function calls, and pointer arithmetic

## 0.1.0 - 2026-04-30

Initial release of the Rux compiler and package manager.

> **Note:** This release supports compiling simple `Main` functions with arithmetic return expressions only. Full language features are not yet implemented.

```rux
func Main() -> int32 {
    return 10 + 2 * (5 - 3);
}
```

### Added

#### Compiler Pipeline

- **Lexer** — tokenizes `.rux` source files; reports diagnostics with file, line, and column; supports token stream dump (`--dump-tokens`)
- **Parser** — produces an AST from the token stream; supports AST dump (`--dump-ast`)
- **Semantic analysis** — type checking and name resolution; supports analysis dump (`--dump-sema`)
- **HIR** — high-level intermediate representation lowered from the AST
- **LIR** — low-level intermediate representation; supports dump (`--dump-lir`)
- **ASM** — x86-64 assembly emitter; supports dump (`--dump-asm`)
- **RCU** — native object file emitter; supports dump (`--dump-rcu`)
- **Linker** — links RCU object files into a native executable

#### CLI / Package Manager

- `rux build` — compile the current package; supports only `--dump-*` flags
- `rux run` — build and execute the package binary
- `rux new <name>` — scaffold a new package in a new directory only (`--bin` / `--lib`)
- `rux version` — print compiler version and build timestamp
- `rux help [command]` — show help for a command
- `-V` / `--version` — global flag that prints the version

[Unreleased]: https://github.com/rux-lang/Rux/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/rux-lang/Rux/releases/tag/v0.3.0
