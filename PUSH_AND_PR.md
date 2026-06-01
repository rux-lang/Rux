# Push and open PR (feature/windows-dll → dev)

Git was not available in the build environment. Run these commands locally after installing [Git](https://git-scm.com/download/win).

## One-time setup

```powershell
cd C:\Users\Rani\Desktop\rux
git init
git remote add upstream https://github.com/rux-lang/Rux.git
git remote add origin https://github.com/YOUR_GITHUB_USER/Rux.git
git fetch upstream dev
git checkout -b feature/windows-dll
```

If you already cloned your fork, copy the changed files from this folder into your clone instead of `git init`.

## Commit

```powershell
git add Include/Rux/Linker.h Include/Rux/Manifest.h Source/Linker.cpp Source/Cli.cpp `
  Tests/Dll Tests/run_dll_test.sh .github/workflows/windows.yml CHANGELOG.md
git commit -m "Add Windows PE32+ DLL output for Type=Dll packages"
```

## Push and PR

```powershell
git push -u origin feature/windows-dll
gh pr create --repo rux-lang/Rux --base dev --head YOUR_GITHUB_USER:feature/windows-dll `
  --title "Add Windows PE32+ DLL output for Type=Dll packages" `
  --body-file PR_BODY.md
```

Target branch must be **`dev`**, not `main` (see CONTRIBUTING.md).
