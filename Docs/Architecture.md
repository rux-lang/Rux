# Compiler architecture

Rux is split into focused CMake component targets whose dependencies follow the compilation pipeline. Most implementation folders map directly to a target; header-only target models are shared by the components that consume them. This keeps frontend, backend, host-system, and package-management changes reviewable without turning `RuxCore` into a monolith.

## Compilation pipeline

```text
Source -> Lexer -> Syntax -> SemanticModel
                                |
                                v
                          AST-to-HIR -> HIR passes
                                |
                                v
                          HIR-to-LIR -> LIR
                                |
                                v
                         x86-64 CodeGen -> RCU Object -> Linker
```

The driver loads the root manifest and dependencies before entering this pipeline. Diagnostics can stop the process after any frontend stage; object emission and linking only run when analysis and lowering succeed.

## Component ownership

| Component              | Owns                                                                  | May depend on                         |
| ---------------------- | --------------------------------------------------------------------- | ------------------------------------- |
| `Diagnostics`          | Diagnostic values and rendering primitives                            | Standard library only                 |
| `Source`               | Source loading and source locations                                   | Diagnostics                           |
| `System`               | Host OS, process, filesystem, networking, and environment access      | Standard library and host APIs        |
| `Target`               | Header-only target triples, ABI, layout, and instruction models       | Standard library only                 |
| `Package`              | `Rux.toml`, dependency metadata, and workspace discovery              | Standard library                      |
| `Lexer`                | Tokens and lexical analysis                                           | Diagnostics                           |
| `Syntax`               | AST and parser                                                        | Lexer and Diagnostics                 |
| `Semantic`             | Symbols, types, conditional compilation, and validated semantic model | Syntax and Diagnostics                |
| `Ir/Hir`               | High-level IR and its transformations                                 | Semantic and Lexer                    |
| `Ir/Lir`               | Control-flow-explicit low-level IR                                    | Semantic                              |
| `Lowering`             | AST/semantic model → HIR → LIR                                        | Frontend and IR components            |
| `CodeGen/X86_64`       | x86-64 code generation and RCU construction                           | LIR, Object, Diagnostics              |
| `Object/Rcu`           | RCU object representation and serialization                           | Standard library                      |
| `Linker`               | PE, ELF, and Mach-O output                                            | Object and System                     |
| `Driver`               | End-to-end compilation orchestration and build reports                | All compiler stages                   |
| `Formatter` / `Linter` | Source formatting and lint diagnostics                                | Syntax; the linter also uses Semantic |

The CMake target graph enforces these dependencies. Prefer adding a dependency to the narrowest owning component rather than reaching through `RuxCore`.

## Host and target boundaries

`Target` describes the output machine. `System` describes the host running the compiler. Code generation and linking must use target data rather than host preprocessor checks. For example, emitting a Linux executable while running on Windows is a target decision; locating `%LocalAppData%` is a host decision.

Operating-system APIs are confined to `Compiler/System`; the CI isolation guard is `Tests/Policy/PlatformIsolation/Check.sh`. New uses of `getenv`, `<windows.h>`, `fork`, or similar APIs belong behind a `System` interface.

## Namespaces and public boundaries

The principal ownership namespaces currently enforced at cross-platform and orchestration boundaries are `Rux::Target`, `Rux::System`, and `Rux::Driver`. New standalone tools use `Rux::Formatting` and `Rux::Linting`. Existing language model types remain in `Rux` while those large APIs are migrated incrementally; new code must not add declarations to `Misc` or recreate a generic `Utils` component.

The build exposes focused targets such as `RuxSyntax`, `RuxSemantic`, `RuxHir`, `RuxLir`, `RuxLowering`, `RuxCodeGenX86_64`, `RuxObjectRcu`, `RuxLinker`, and `RuxDriver`. `RuxCore` is an interface-only compatibility aggregation target.

`RuxCore` is convenient for the unit-test executable and embedders, but compiler components must link to their actual dependencies. It must not become a shortcut that introduces cycles between stages.

## CLI and package flows

Formatter and linter are internal compiler components exposed through the single `rux` application:

```text
rux fmt  -> RuxFormatter -> RuxSyntax
rux lint -> RuxLinter    -> RuxSyntax + RuxSemantic
rux      -> RuxDriver
```

Package commands use `Package/Manifest` for manifest parsing and `System/Process` for registry/network operations. Build and check resolve path dependencies directly and registry dependencies from the shared package cache:

- Windows: `%LocalAppData%\Rux\Packages`
- Unix-like hosts: `~/.rux/packages`

An explicit `[Workspace]` manifest names its member packages. Workspace checks resolve dependencies matching member package names from the local source tree. `rux test` discovers runnable packages below the root `Tests/` tree, requires their direct dependencies to use local path entries, resolves transitive first-party dependencies from workspace members, and disables registry fallback. Publishable package manifests can therefore retain registry-compatible dependency declarations without making repository tests depend on the network or shared package cache.

## Failure and diagnostic contracts

Ordinary compiler failures are values, not exceptions. Frontend stages collect diagnostics with source locations so users can fix several problems per run. Filesystem, process, and network helpers return `bool`, `std::optional`, or a result object; the CLI owns user-facing error text and exit codes.

When adding a stage or pass:

1. Put the implementation in the component that owns the data it transforms.
2. Add the narrow CMake dependency required by that implementation.
3. Preserve diagnostics instead of printing from library code.
4. Add focused C++ unit coverage, plus a language integration test when user behavior changes.
5. Update this document if ownership or dependency direction changes.
