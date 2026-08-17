<!--
Target branch must be `dev`. Pull requests against `main` are closed automatically. See Docs/PullRequest.md for the full lifecycle.
-->

## What and Why

<!-- What changed, and what problem it solves. Link the issue: Fixes #42 -->

## Verification

<!-- The exact commands you ran, and anything you could not run locally (e.g. a platform you don't have). -->

```sh
```

## Checklist

- [ ] Branched from `dev` and targeting `dev`
- [ ] `sh Run.sh test --clang-tidy` / `./Run.ps1 test -ClangTidy` passes locally — policy guards, Release build, formatting, static analysis, C++ unit tests, workspace check and lint, and every Rux test package
- [ ] Tests added or updated: integration/golden for user-visible behavior, C++ unit for internals
- [ ] `CHANGELOG.md` and affected documentation updated
