# Contributing to Rux

Thank you for your interest in contributing to the Rux programming language!

## Ways to Contribute

- Report bugs via [GitHub Issues](https://github.com/rux-lang/Rux/issues)
- Propose language features in [GitHub Discussions](https://github.com/rux-lang/Rux/discussions)
- Submit pull requests for bug fixes or approved features
- Improve documentation

## Getting Started

1. Fork the repository at <https://github.com/rux-lang/Rux/fork>
2. Clone your fork and create a feature branch:
   ```sh
   git checkout -b my-feature
   ```
3. Make your changes, then commit:
   ```sh
   git commit -am "Short description of change"
   ```
4. Push your branch and open a Pull Request against `dev`.

> [!Note]
> All contributions should target the `dev` branch. Changes are reviewed and tested there before being merged into `main` for releases.

## Pull Request Guidelines

- Keep PRs focused — one logical change per PR.
- Reference the relevant issue (e.g., `Fixes #42`) in the PR description.
- Ensure existing tests pass before submitting.
- Add tests for any new behavior.

## Commit Messages

Use short imperative sentences: `Fix parser crash on empty block`, not `Fixed the parser`.

## Code Style

Follow the conventions already present in the codebase. Consistency matters more than personal preference.

## Testing

### Running the test suite

From the root of any Rux project that has a `Tests/` directory, run:

```sh
rux test
```

`rux test` automatically discovers every subdirectory of `Tests/` that contains a `Rux.toml` with `Type = "bin"`, builds it, executes it, and reports a per-package PASS/FAIL line followed by an overall summary:

```
     Testing MyProject v0.1.0
      Running test package: BoolBitwise
    PASS: BoolBitwise
      Running test package: Pow
    PASS: Pow
ok: 2 passed, 0 failed, 2 total
```

Pass `--release` to test against the optimised build and `--verbose` to see
the path of each binary being executed.

### Test package layout

Each test package lives under `Tests/<Name>/` and must contain:

```
Tests/
  MyFeature/
    Rux.toml        # [Package] Type = "bin"
    Src/
      Main.rux      # returns 0 on success, non-zero on failure
```

The binary's exit code is the sole signal: **0 = PASS, anything else = FAIL**.
This lets test packages assert arbitrary conditions without any test framework:

```rux
func Main() -> int {
    // returning a non-zero value here will fail the test
    if 2 ** 10 != 1024 { return 1; }
    return 0;
}
```

### Shell-based integration tests

For tests that require stdin/stdout matching or multi-binary orchestration,
shell scripts are provided in `Tests/`. Each script is self-contained and
accepts a path to the `rux` binary as its first argument (or via `$RUX`):

```sh
RUX=./build/rux Tests/run_io_test.sh
RUX=./build/rux Tests/run_bool_bitwise_test.sh
```

These scripts serve as regression guards and are run in CI. When adding a new
language feature, add a matching test package **and** a shell script if the
feature requires output comparison.

## Reporting Bugs

Include:

- Rux version / commit hash (`rux version`)
- Minimal reproducer (source file or snippet)
- Expected vs. actual behavior

## Community

Join the discussion on [Discord](https://discord.com/invite/uvSHjtZSVG) or [GitHub Discussions](https://github.com/rux-lang/Rux/discussions) if you have questions before diving in.

## License

By contributing you agree that your work will be licensed under the [MIT License](LICENSE).
