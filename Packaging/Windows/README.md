# Rux on Windows

Two ways to install the Rux compiler on Windows, each in its own folder:

| Folder                       | Channel              | Best for                                              |
| ---------------------------- | -------------------- | ----------------------------------------------------- |
| [`Msi/`](Msi/)               | MSI installer        | A familiar wizard + Add/Remove Programs entry.        |
| [`PowerShell/`](PowerShell/) | `install.ps1` script | One-line install from a terminal (no download click). |

Both install per-user (**no admin / UAC prompt**): they drop `rux.exe` under `%LocalAppData%\Programs\Rux` and add it to the user `PATH`.

## Quick start

```powershell
# PowerShell script (downloads the latest release):
irm https://rux-lang.dev/install.ps1 | iex

# ...or grab rux-windows.msi from the GitHub Release and double-click it.
```

After installing, open a **new** terminal so the updated `PATH` takes effect, then run `rux help`.

See each folder's `README.md` for build instructions and details.
