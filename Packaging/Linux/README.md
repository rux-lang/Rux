# Rux Linux Installer

A per-user install script that downloads a Rux release from GitHub, installs the `rux` binary, and makes it available from any terminal.

## What it does

- Downloads the latest (or a pinned) `rux-linux.tar.gz` from the [GitHub Releases](https://github.com/rux-lang/Rux/releases).
- Installs `rux` to `~/.local/bin` (**no root / sudo**).
- Adds that directory to your `PATH` via your shell rc file if it isn't there already (skippable with `--no-modify-path`).
- Works with either `curl` or `wget`, in any POSIX `sh`.

## Quick install

```sh
curl -fsSL https://rux-lang.dev/install.sh | sh
```

Then open a **new** terminal so the updated `PATH` takes effect and run `rux help`.

## Options

Run the script directly to pass options (also settable via environment variables for the piped form):

```sh
./install.sh [--version X.Y.Z] [--dir DIR] [--no-modify-path]
```

| Option             | Env var              | Default        | Purpose                         |
| ------------------ | -------------------- | -------------- | ------------------------------- |
| `--version X.Y.Z`  | `RUX_VERSION`        | latest release | Install a specific version.     |
| `--dir DIR`        | `RUX_INSTALL_DIR`    | `~/.local/bin` | Install directory.              |
| `--no-modify-path` | `RUX_NO_MODIFY_PATH` | unset          | Don't touch any shell rc files. |

Examples:

```sh
# Pin a version
RUX_VERSION=0.3.0 curl -fsSL .../install.sh | sh

# System-wide install (requires write access to the target dir)
sudo ./install.sh --dir /usr/local/bin --no-modify-path
```

## Layout

| File         | Purpose                   |
| ------------ | ------------------------- |
| `install.sh` | The POSIX `sh` installer. |

## Notes

- Currently, only the `x86_64` Linux binary is published; the script errors out on other architectures and points to building from source.
- The script is served straight from the repo — there is nothing to build, and the release workflow attaches the `rux-linux.tar.gz` it consumes.
