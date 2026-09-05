# Building and Measuring the Compiler

Rux requires upstream Clang 23.1 or newer, CMake 4.4.3+, Ninja 1.13.2+, and C++26. Use LLVM 23's `clang-format` and `clang-tidy` for consistent
formatting and analysis. `.clang-format`, `.editorconfig`, and Git attributes specify LF on every host. These settings
build the C++ compiler; Rux programs continue to use Rux's own encoders, object writers, and linkers.

## Incremental C++ Builds

Configure once with `./Run.ps1 build` or `sh Run.sh build`, then use `cmake --build Build --config Release`. Component
source lists live beside their implementations. Shared types, tokens, AST data, semantic results, and driver events
have focused targets so backend and presentation code do not acquire parser or analyzer implementation dependencies.
Offline Unicode and numeric-vector generators are excluded from ordinary builds; their explicit targets remain
available.

Compiler version and timestamp values are defined in one generated implementation unit. Reconfiguring an unchanged
tree does not rewrite a public header or rebuild its consumers. A build tree retains its initial UTC timestamp in
`RUX_INITIAL_BUILD_TIMESTAMP`. `SOURCE_DATE_EPOCH` takes precedence when present; an explicit
`-DRUX_BUILD_TIMESTAMP=2026-09-05T00:00:00Z` takes precedence over both. A metadata change rebuilds the implementation
unit and relinks its consumers. This does not introduce a persistent incremental cache for Rux programs.

## Optional Build Settings

| CMake setting | Behavior |
| --- | --- |
| `CMAKE_CXX_COMPILER_LAUNCHER` | Standard CMake launcher support, for example `ccache` or `sccache`; unset by default |
| `RUX_USE_PCH` | Precompile standard-library headers for compiler components and a separate standard-library/doctest header for unit tests |
| `RUX_DEAD_STRIP` | Put optimized functions/data in discardable sections and request the platform's unused-section elimination |
| `RUX_THIN_LTO` | Optional ThinLTO for optimized builds, with a linker cache under the selected build tree's `ThinLTO/` |

The PCH contains no project headers or generated metadata. The doctest implementation translation unit does not use
the unit PCH. `Run.ps1 tidy` and `Run.sh tidy` automatically configure a PCH-disabled database under `Build/Analysis`
when the development build uses PCH; `cmake --build Build --target rux-analysis-database` prepares it explicitly.
PCH defaults on for native Windows x86-64 builds using the GNU Clang frontend; other hosts and frontends default off.
Unused-section elimination and ThinLTO remain opt-in for ordinary development. RTTI, exceptions, supported features, and the existing Windows
runtime selection remain enabled/unchanged.

For example, opt into PCH in an existing build:

```sh
cmake -S . -B Build -DRUX_USE_PCH=ON
cmake --build Build --config Release
```

Use a separate tree for experimenting with ThinLTO or size options. Build trees in the same checkout share `Bin/`,
so do not build them concurrently. The last successful link supplies the executable used by repository commands.

CI uses compilation caches separated by operating system, target architecture, Clang major, runtime, and Release
configuration. Compiler contents are checked by the cache itself. CI cache builds explicitly disable PCH, avoiding
relaxed cache correctness settings for precompiled headers. A launcher only caches C++ compilation; it does not cache
Rux package builds or test results.

## Repeatable Measurements

Run the measurement helper inside the same toolchain environment used to configure the selected build:

```sh
python Scripts/MeasurePerformance.py --build Build --clean --tests --repetitions 3 --jobs 4
python Scripts/MeasurePerformance.py --build Build --rux-suite --rux-jobs 4 --repetitions 3 --jobs 4
```

The helper records clean builds (when requested), unchanged builds, reconfiguration, implementation-only and shared
type-header rebuilds, representative Rux compilation, optional C++/Rux suites, executable bytes/hash, and section
sizes when `llvm-size` is available. JSON includes the toolchain cache, host, processor count, and every timing sample;
command output is saved beside it. It supplies a fixed `SOURCE_DATE_EPOCH` unless the caller already set one. Source
modification times touched for rebuild probes are restored.

Compare at least three repetitions under matching toolchain, runtime, configuration, worker count, and host load.
Keep cold and warm compilation caches distinct. Enable an optional setting by default only when its intended metric
improves at least 5% and other measured metrics have no repeatable regression over 3%. Native execution coverage
requires a compatible OS and architecture; cross-target artifact comparison does not establish native runtime
coverage.

## Windows x86-64 Measurements

Three repetitions used upstream Clang 23.1.0, CMake 4.4.3, Ninja 1.13.2, Release, the existing static Windows runtime,
16 C++ build workers, and no compilation launcher. The original source is commit `4e1a69e0`. Both source trees used
`SOURCE_DATE_EPOCH=1788566400`; reconfiguration numbers therefore describe reproducible metadata in both trees.
Ordinary reconfiguration also leaves the refactored build unchanged without that environment variable.

| Median elapsed time | Original | Refactored, PCH off | Refactored, PCH on |
| --- | ---: | ---: | ---: |
| Clean compiler and unit-test build | 93.136 s | 108.734 s | 72.493 s |
| Unchanged rebuild | 0.053 s | 0.052 s | 0.053 s |
| Reconfiguration | 0.168 s | 0.262 s | 0.267 s |
| Rebuild after reconfiguration | 0.044 s | 0.053 s | 0.050 s |
| Semantic implementation rebuild | 6.419 s | 3.385 s | 2.430 s |
| Shared type-header rebuild | 55.218 s | 65.877 s | 42.480 s |
| C++ suite, up to four CTest workers | 17.837 s | 15.095 s | 13.681 s |

PCH reduced clean builds by 33.3% versus the refactored PCH-disabled build and 22.2% versus the original. The semantic
implementation probe touches the former analyzer or the extracted type-resolution file; the shared-header probe
uses the resolved type model in both revisions. The original suite had 1,464 tests; the refactored suite adds focused
regressions and verifies that its CTest groups cover every registration exactly once. Test timings have host noise;
all samples are retained by the measurement helper rather than selecting the fastest run.

The small Arithmetic compilation probe had a cold first launch near 0.9 seconds, followed by 11–24 ms warm launches.
Those short samples are not sufficient to claim a Rux compiler throughput improvement.

With PCH, optional unused-section elimination produced a 6,799,360-byte executable, compared with 6,990,336 bytes
without it and 6,980,096 bytes originally. The 2.7% reduction versus the refactored build is below the 5% threshold,
so this option remains disabled by default. Its clean-build median was 70.930 seconds and C++ suite median was
14.009 seconds. Windows already discards unreferenced COMDATs and folds identical code; separate function/data
sections allow additional unused code to be discarded without removing supported features.
