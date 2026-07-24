<!--
Target branch must be `dev`. Pull requests against `main` are closed automatically.
See Docs/PullRequest.md for the full lifecycle.
-->

## What and Why

<!-- What changed, and what problem it solves. Link the issue: Fixes #42 -->

## Verification

<!-- The exact commands you ran, and anything you could not run locally (e.g. a platform you don't have). -->

```sh
```

## Checklist

- [ ] Branched from `dev` and targeting `dev`
- [ ] Release build passes locally
- [ ] `rux test --release` passes from the repository root
- [ ] `ctest --test-dir Build --output-on-failure -C Release` passes (compiler internals)
- [ ] Sources formatted with `sh Format.sh` / `./Format.ps1`
- [ ] `clang-tidy` clean for touched translation units (`--clang-tidy` / `-ClangTidy`)
- [ ] Tests added or updated: integration/golden for user-visible behavior, C++ unit for internals
- [ ] `CHANGELOG.md` and affected documentation updated
