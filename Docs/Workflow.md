# Development Workflow

The day-to-day loop for changing the Rux compiler. For where your branch should
come from and where it goes, see [Branch Architecture](Branches.md).

## 1. Prerequisites

Install the toolchain (Clang 22.1+, CMake 4.3+, Ninja 1.13+, Git 2.54+) per your
OS — see [Building from Source](../README.md#building-from-source).

> _TODO: note any extra tooling contributors need beyond the README list
> (e.g. `clang-format`, `clang-tidy`, a specific debugger, editor setup)._

## 2. Get the source and configure

```sh
git clone https://github.com/rux-lang/Rux.git
cd Rux
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++   # adjust compiler name per OS
```

## 3. The inner loop

1. **Branch** off `dev` (see [Branch Architecture](Branches.md)).
2. **Edit** sources under `Source/` and `Include/`.
3. **Build** the incremental build:
   ```sh
   cmake --build build --config Release
   ```
4. **Run / debug** the compiler you just built:
   ```sh
   ./build/rux help        # Windows: .\build\rux.exe help
   ```
5. **Test** (see below).
6. **Format** touched files: `clang-format -i <files>`.

> _TODO: document Debug vs. Release builds and when to use each
> (`-DCMAKE_BUILD_TYPE=Debug`), plus any sanitizer presets._

## 4. Repository layout

| Path             | Purpose                                   |
|------------------|-------------------------------------------|
| `Source/`        | Compiler implementation (`.cpp`)          |
| `Include/`       | Public/internal headers (`.h`)            |
| `Tests/`         | Test packages, one per subdirectory       |
| `Bin/`           | _TODO: describe_                          |
| `CMakeLists.txt` | Build configuration and project `VERSION` |

> _TODO: expand into the compiler's internal stages (lexer → parser → … →
> codegen) and which directory owns each, so newcomers know where to make a
> change._

## 5. Testing

Run the full suite from the repo root:

```sh
./build/rux install Std     # test packages depend on Std
./build/rux test            # add --release to test the optimized build, --verbose for paths
```

`rux test` discovers every subdirectory of `Tests/`, builds and runs each
package, and reports per-package results plus a summary. A test package is a
normal binary package whose **exit code is the only signal: `0` = pass, anything
else = fail** — no framework required:

```rux
func Main() -> int {
    if 2 * 10 != 20 {
        return 1;
    }
    return 0;
}
```

Each package lives at `Tests/<Name>/` with a `Rux.toml` manifest and
`Src/Main.rux`:

```toml
[Package]
Name = "Arithmetic"
Version = "0.1.0"
Description = "Test for Rux compiler"

[Build]
Output = "../Bin"
```

**When you add a language feature, add a matching test package under `Tests/`.**

## 6. Code style

Formatting is enforced by [`.clang-format`](../.clang-format) (LLVM base,
4-space indent, west const, 120-column limit).

```sh
clang-format -i $(git ls-files '*.cpp' '*.h')
```

> _TODO: capture naming conventions, header/include ordering, error-handling
> patterns, and anything `clang-format` can't enforce._

## 7. Commits

- One logical change per commit; keep history readable.
- Write messages as short imperative sentences:
  `Fix parser crash on empty block`, not `Fixed the parser`.

> _TODO: state whether commits are squashed on merge, and any required
> trailers (e.g. `Fixes #42`)._
