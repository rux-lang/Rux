# Rux on Windows

Two ways to install the Rux compiler on Windows, each in its own folder:

| Folder                                | Channel              | Best for                                                           |
| ------------------------------------- | -------------------- | ------------------------------------------------------------------ |
| [`Msi/`](Msi/README.md)               | MSI installer        | Interactive or silent deployment with an Add/Remove Programs entry |
| [`PowerShell/`](PowerShell/README.md) | `install.ps1` script | One-line terminal install, version pinning, or a custom directory  |

Both install per-user (**no admin / UAC prompt**): they drop `rux.exe` under `%LocalAppData%\Programs\Rux` and add it to the user `PATH`.

## Quick start

```powershell
# PowerShell script (downloads the latest release):
irm https://rux-lang.dev/install.ps1 | iex

# ...or grab rux-windows.msi from the GitHub Release and double-click it.
```

After installing, open a **new** terminal so the updated `PATH` takes effect, then run:

```powershell
rux version
```

Re-run either installer to upgrade in place. Use the same channel for upgrades so uninstall metadata and PATH ownership remain predictable.

See each folder's `README.md` for build instructions and details.

The official [Scoop bucket](https://github.com/rux-lang/Scoop) provides another Windows installation channel, but its manifest and publishing automation live in that separate repository.
