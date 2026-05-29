# Homebrew packaging

`rux.rb` is the source-of-truth Homebrew formula for the `rux` compiler. It
builds from source via CMake using Homebrew's LLVM (Apple's bundled clang is
too old for Rux's C++26 sources).

Homebrew formulae live in a **tap** repository, not in this repo, so the steps
below copy `rux.rb` into a tap.

## Prerequisites: cut a release that contains the macOS backend

The Mach-O linker backend landed **after** the `v0.2.2` tag, so an existing
release won't include it. Cut a new tag from a commit that has the backend:

```sh
# Optionally bump CMakeLists.txt project(... VERSION x.y.z) to match the tag
# so `rux --version` reports it.
git tag v0.3.0
git push origin v0.3.0     # push to the canonical repo (rux-lang/Rux)
```

Then create a GitHub release for that tag (the formula points at the
auto-generated source tarball).

## Fill in the formula

1. Set `url` in `rux.rb` to the release tarball, e.g.
   `https://github.com/rux-lang/Rux/archive/refs/tags/v0.3.0.tar.gz`.
2. Compute and paste the checksum:

   ```sh
   curl -L https://github.com/rux-lang/Rux/archive/refs/tags/v0.3.0.tar.gz | shasum -a 256
   ```

## Create the tap

```sh
# Create a repo named exactly `homebrew-rux` under the rux-lang org, then:
git clone https://github.com/rux-lang/homebrew-rux.git
mkdir -p homebrew-rux/Formula
cp rux.rb homebrew-rux/Formula/rux.rb
cd homebrew-rux && git add Formula/rux.rb && git commit -m "Add rux formula" && git push
```

## Verify and install

```sh
brew install --build-from-source ./rux.rb   # local smoke test before pushing
brew test ./rux.rb
brew audit --new --formula ./rux.rb          # lint before publishing

# After the tap is pushed, end users install with:
brew install rux-lang/rux/rux
```

## Notes

- Produced executables are **x86-64 only**; on Apple Silicon they run via
  Rosetta 2. The `rux` compiler itself builds and runs natively on arm64.
- `head "...", branch: "main"` lets `brew install --HEAD rux` build from the
  tip of `main` for testing unreleased changes.
