## Summary

- Adds Windows PE32+ DLL linking when `Rux.toml` sets `Type = "Dll"` (or `dll`).
- `Linker` accepts `isDll`; PE backend emits export directory, DLL characteristics, and optional `DllMain` entry.
- `rux build` writes `Bin/<Profile>/<Name>.dll` on Windows; `rux run` rejects DLL packages.
- Documents `rux install --dev` in help text and `Dll` package type in `Manifest.h`.
- Adds `Tests/Dll` and Windows CI step `Tests/run_dll_test.sh`.

## Limitations

- DLL output is **Windows PE32+ only** in this change (no ELF `.so` / Mach-O `.dylib` yet).
- Exported symbols: public (`pub`) functions except `DllMain`; imports resolve via PE export tables on search path.
- End-to-end DLL *loading* from Rux (function-pointer calls) is still limited; see [Rux.DllExamples](https://github.com/YOUR_GITHUB_USER/Rux.DllExamples) for build/load notes.

## Test plan

- [ ] Windows CI: `Tests/run_dll_test.sh` produces `Tests/Dll/Bin/Debug/dll_test.dll`
- [ ] `Tests/run_io_test.sh`, `run_pow_test.sh`, `run_unicode_test.sh` still pass (EXE regression)
- [ ] `dumpbin /exports dll_test.dll` lists `Add`
- [ ] Optional: build `Rux.DllExamples/BasicDll` with `rux install --dev Std` / `Windows`
