@echo off
REM ============================================================
REM  Rux v0.3.0 — Create Pull Request Script
REM  GitHub username: rootahmad
REM ============================================================

echo.
echo ========================================
echo  Rux v0.3.0 — Pull Request Setup
echo ========================================
echo.

cd /d "%~dp0"

REM Step 1: Initialize git if needed
if not exist ".git" (
    echo [1/6] Initializing git repository...
    git init
    git checkout -b main
) else (
    echo [1/6] Git repository already exists.
)

REM Step 2: Configure git (optional)
echo [2/6] Setting git identity...
git config user.name "rootahmad"
git config user.email "rootahmad@users.noreply.github.com"

REM Step 3: Stage all files
echo [3/6] Staging all files...
git add -A

REM Step 4: Commit
echo [4/6] Creating commit...
git commit -m "feat: Rux v0.3.0 — lambdas, string interpolation, optional chaining, pipeline, try/catch, defer, optional types" -m "Major language feature release adding 8 new features:" -m "- Lambda/closure expressions with |params| expr syntax" -m "- String interpolation with f"..." syntax" -m "- Optional chaining (?.) and null-coalescing (??) operators" -m "- Pipeline operator (|>) for function composition" -m "- try/catch structured error handling" -m "- defer statements for scope-exit cleanup" -m "- Optional type syntax (T?)" -m "- 6 new test programs"

REM Step 5: Add remote
echo [5/6] Adding remote origin...
git remote remove origin 2>/dev/null
git remote add origin https://github.com/rootahmad/Rux.git

REM Step 6: Push
echo [6/6] Pushing to GitHub...
git push -u origin main

echo.
echo ========================================
echo  Done! Now create the PR:
echo ========================================
echo.
echo  1. Go to: https://github.com/rux-lang/Rux/compare
echo  2. Click "compare across forks"
echo  3. Select head repo: rootahmad/Rux
echo  4. Click "Create Pull Request"
echo.
echo  Or run this command:
echo  gh pr create --repo rux-lang/Rux --title "feat: Rux v0.3.0" --body "Adds lambda expressions, string interpolation, optional chaining, null-coalescing, pipeline operator, try/catch, defer, and optional types"
echo.
pause
