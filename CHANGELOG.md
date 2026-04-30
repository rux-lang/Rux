# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2026-04-30

Initial release of the Rux compiler and package manager.

> **Note:** This release supports compiling simple `Main` functions with arithmetic return expressions only. Full language features are not yet implemented.

```rux
func Main() -> int32 {
    return 10 + 2 * (5 - 3);
}
```

### Compiler pipeline

- **Lexer** — tokenizes `.rux` source files; reports diagnostics with file, line, and column; supports token stream
  dump (`--dump-tokens`)
- **Parser** — produces an AST from the token stream; supports AST dump (`--dump-ast`)
- **Semantic analysis** — type checking and name resolution; supports analysis dump (`--dump-sema`)
- **HIR** — high-level intermediate representation lowered from the AST
- **LIR** — low-level intermediate representation; supports dump (`--dump-lir`)
- **ASM** — x86-64 assembly emitter; supports dump (`--dump-asm`)
- **RCU** — native object file emitter; supports dump (`--dump-rcu`)
- **Linker** — links RCU object files into a native executable

### CLI commands

- `rux build` — compile the current package; supports only `--dump-*` flags
- `rux run` — build and execute the package binary
- `rux new <name>` — scaffold a new package in a new directory only (`--bin` / `--lib`)
- `rux version` — print compiler version and build timestamp
- `rux help [command]` — show help for a command

### Global flags

- `-V` / `--version` — print version
