# Rux Linux Installer

A per-user installer that downloads a published Rux release from GitHub,
installs the `rux` binary, and makes it available from any terminal.

## What it does

- Downloads the latest (or a pinned) `rux-linux.tar.gz` from the [GitHub Releases](https://github.com/rux-lang/Rux/releases).
- Installs `rux` to `~/.local/bin` (**no root / sudo**).
- Adds that directory to your `PATH` via your shell rc file if it isn't there already (skippable with `--no-modify-path`).
- Works with either `curl` or `wget`, in any POSIX `sh`.

## Quick install

```sh
curl -fsSL https://rux-lang.dev/install.sh | sh
```

Then open a **new** terminal so the updated `PATH` takes effect and verify the
installation:

```sh
rux version
```

## Options

Download the script when you need command-line options:

```sh
curl -fsSLO https://raw.githubusercontent.com/rux-lang/Rux/main/Packaging/Linux/install.sh
chmod +x install.sh
./install.sh [--version X.Y.Z] [--dir DIR] [--no-modify-path]
```

| Option             | Env var              | Default        | Purpose                         |
| ------------------ | -------------------- | -------------- | ------------------------------- |
| `--version X.Y.Z`  | `RUX_VERSION`        | latest release | Install a specific version.     |
| `--dir DIR`        | `RUX_INSTALL_DIR`    | `~/.local/bin` | Install directory.              |
| `--no-modify-path` | `RUX_NO_MODIFY_PATH` | unset          | Don't touch any shell rc files. |
| `--help`           | —                    | —              | Show usage and exit.             |

Examples:

```sh
# Pin a version
curl -fsSL https://rux-lang.dev/install.sh | RUX_VERSION=0.3.0 sh

# Install elsewhere without editing a shell rc file
./install.sh --dir "$HOME/bin" --no-modify-path

# System-wide install (requires write access to the target dir)
sudo ./install.sh --dir /usr/local/bin --no-modify-path
```

## Layout

| File         | Purpose                   |
| ------------ | ------------------------- |
| `install.sh` | The POSIX `sh` installer. |

## Requirements and behavior

The script requires an x86-64 Linux host, `tar`, and either `curl` or `wget`.
It downloads into a temporary directory, verifies that the archive contains a
`rux` binary, and replaces the destination binary only after extraction. It
does not install the Rux package cache; `rux install` manages language-package
dependencies separately.

The default PATH update is idempotent and targets the current login shell:
`.bashrc`, `.zshrc`, Fish's `config.fish`, or `.profile`. Use
`--no-modify-path` when shell configuration is managed elsewhere.

## Upgrade and uninstall

Run the installer again to replace the existing binary with the latest or a
pinned release. To uninstall the default per-user installation:

```sh
rm "$HOME/.local/bin/rux"
```

If the installer added a PATH line, remove the adjacent `Added by the Rux
installer` entry from the shell rc file named during installation.

## Notes

- Currently, only the x86-64 Linux binary is published; unsupported
  architectures receive a build-from-source error.
- The script is served directly from this repository. The release workflow
  attaches the `rux-linux.tar.gz` asset it consumes.
