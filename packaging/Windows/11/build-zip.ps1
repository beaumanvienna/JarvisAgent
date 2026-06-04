# build-zip.ps1 — Build a portable .zip package for JarvisAgent on Windows.
#
# Usage:
#   cd packaging\Windows\11
#   .\build-zip.ps1 [-DryRun]
#
# Prerequisites:
#   - Visual Studio 2022 with C++ workload (MSBuild on PATH)
#   - premake5 on PATH
#   - Python 3 on PATH
#   - Node.js + npm on PATH
#   - MSYS2 or Git Bash (required at runtime for shell tasks)

param(
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot  = (Resolve-Path "$ScriptDir\..\..\..").Path
$PkgVersion = (Select-String -Path "$RepoRoot\premake5.lua" -Pattern 'JARVIS_AGENT_VERSION' |
    ForEach-Object { $_.Line -replace '.*\\"([^"]*)\\".*','$1' } | Select-Object -First 1)
if (-not $PkgVersion) { Write-Host "ERROR: could not extract version from premake5.lua"; exit 1 }
$BuildDir  = "$ScriptDir\build"
$PkgName   = "JarvisAgent-${PkgVersion}-x64"
$StageDir  = "$BuildDir\$PkgName"

if ($DryRun) {
    Write-Host "==> DRY RUN: skipping C++ and React builds, using existing artifacts"
}

# ---- Clean previous build ----
if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

# ---- Build from source (unless dry-run) ----
if (-not $DryRun) {
    Push-Location $RepoRoot

    Write-Host "==> Building Engine edition ..."
    premake5 vs2022 --engine
    $slnFiles = Get-ChildItem -Path $RepoRoot -Recurse -Filter *.sln
    foreach ($sln in $slnFiles) {
        msbuild "$($sln.FullName)" /m /p:Configuration=Release /p:Platform=x64
    }

    Write-Host "==> Building Studio edition ..."
    premake5 vs2022
    $slnFiles = Get-ChildItem -Path $RepoRoot -Recurse -Filter *.sln
    foreach ($sln in $slnFiles) {
        msbuild "$($sln.FullName)" /m /p:Configuration=Release /p:Platform=x64
    }

    Pop-Location

    Write-Host "==> Building React dashboard ..."
    Push-Location "$RepoRoot\code\frontend\dashboard\ui"
    npm install
    npm run build
    Pop-Location

    Write-Host "==> Building React workflow editor ..."
    Push-Location "$RepoRoot\code\frontend\workflow-editor\ui"
    npm install
    npm run build
    Pop-Location
}

# ---- Assemble package tree ----
Write-Host "==> Assembling package tree ..."

# Binaries (Studio + Engine)
New-Item -ItemType Directory -Force -Path "$StageDir\bin" | Out-Null
foreach ($bin in @("jarvisAgent-studio.exe", "jarvisAgent-engine.exe")) {
    $binSrc = "$RepoRoot\bin\Release\$bin"
    if (Test-Path $binSrc) {
        Copy-Item $binSrc "$StageDir\bin\$bin"
    } else {
        Write-Host "WARNING: bin\Release\$bin not found (expected in dry-run)"
    }
}

# React UIs (built from code\frontend\...; staged at dashboard\ui\dist for the runtime serving layout)
if (Test-Path "$RepoRoot\code\frontend\dashboard\ui\dist") {
    New-Item -ItemType Directory -Force -Path "$StageDir\dashboard\ui" | Out-Null
    Copy-Item -Recurse "$RepoRoot\code\frontend\dashboard\ui\dist" "$StageDir\dashboard\ui\dist"
} else {
    Write-Host "WARNING: dashboard\ui\dist not found (expected in dry-run)"
}

if (Test-Path "$RepoRoot\code\frontend\workflow-editor\ui\dist") {
    New-Item -ItemType Directory -Force -Path "$StageDir\workflow-editor\ui" | Out-Null
    Copy-Item -Recurse "$RepoRoot\code\frontend\workflow-editor\ui\dist" "$StageDir\workflow-editor\ui\dist"
} else {
    Write-Host "WARNING: workflow-editor\ui\dist not found (expected in dry-run)"
}

# Scripts (excluding __pycache__)
Copy-Item -Recurse "$RepoRoot\scripts" "$StageDir\scripts"
Get-ChildItem -Path "$StageDir\scripts" -Recurse -Directory -Filter "__pycache__" |
    Remove-Item -Recurse -Force

# Example workflows (curated list — no subdirs, no build artifacts)
New-Item -ItemType Directory -Force -Path "$StageDir\workflows" | Out-Null
$jcwfFiles = @(
    "aiCarMaintenancePipeline", "aiZipDemo",
    "make-example", "portfolioDividendAnalysis"
)
foreach ($jcwf in $jcwfFiles) {
    $src = "$RepoRoot\example\workflows\$jcwf.jcwf"
    if (Test-Path $src) { Copy-Item $src "$StageDir\workflows\" }
}
# (Input files are bundled inside each .jcwf zip — no loose files to copy)

# Example config
Copy-Item "$RepoRoot\packaging\config.json.example" "$StageDir\config.json.example"

# Runtime directories
New-Item -ItemType Directory -Force -Path "$StageDir\queue" | Out-Null
New-Item -ItemType Directory -Force -Path "$StageDir\log" | Out-Null

# Documentation
New-Item -ItemType Directory -Force -Path "$StageDir\doc" | Out-Null
Copy-Item "$RepoRoot\README.md" "$StageDir\doc\README.md"
if (Test-Path "$RepoRoot\doc\JC_Workflow_Specification.md") {
    Copy-Item "$RepoRoot\doc\JC_Workflow_Specification.md" "$StageDir\doc\JC_Workflow_Specification.md"
}

# Launcher batch file (user-space: creates %USERPROFILE%\JarvisAgent with junctions)
$launcherContent = @'
@echo off
setlocal enabledelayedexpansion
REM jarvisagent.bat — User-space launcher for JarvisAgent on Windows.
REM Creates a per-user working directory with junctions to read-only assets.
REM Junctions (mklink /J) do not require admin on Windows 10+.
REM
REM Edition is detected from the script name:
REM   jarvisagent.bat / jarvisagent-studio.bat / j9t.bat  → Studio edition
REM   jarvisagent-engine.bat                              → Engine edition
REM
REM Usage:
REM   jarvisagent                          default: %USERPROFILE%\JarvisAgent
REM   jarvisagent --home C:\path\to\dir    custom working directory
REM   set JARVISAGENT_HOME=C:\path & jarvisagent   via environment variable
REM   jarvisagent --no-browser             skip opening dashboard in browser

set "INSTALL_DIR=%~dp0"
if "!INSTALL_DIR:~-1!"=="\" set "INSTALL_DIR=!INSTALL_DIR:~0,-1!"
set "OPEN_BROWSER=1"
set "USER_HOME="

REM ---- Detect edition from script name ----
set "BINARY_NAME=jarvisAgent-studio.exe"
set "EDITION_LABEL=Studio"
set "SCRIPT_NAME=%~n0"
echo !SCRIPT_NAME! | findstr /i "engine" >nul 2>&1
if not errorlevel 1 (
    set "BINARY_NAME=jarvisAgent-engine.exe"
    set "EDITION_LABEL=Engine"
)

REM ---- Parse arguments ----
:parse
if "%~1"=="" goto :main
if /i "%~1"=="--home" (
    set "USER_HOME=%~2"
    shift
    shift
    goto :parse
)
if /i "%~1"=="--no-browser" (
    set "OPEN_BROWSER=0"
    shift
    goto :parse
)
if /i "%~1"=="--help" goto :passthrough
if /i "%~1"=="-h" goto :passthrough
if /i "%~1"=="--version" goto :passthrough
if /i "%~1"=="-v" goto :passthrough
shift
goto :parse

:passthrough
"!INSTALL_DIR!\bin\!BINARY_NAME!" %*
exit /b

:main
if "!USER_HOME!"=="" (
    if defined JARVISAGENT_HOME (
        set "USER_HOME=!JARVISAGENT_HOME!"
    ) else (
        set "USER_HOME=%USERPROFILE%\JarvisAgent"
    )
)

REM ---- Verify install directory ----
if not exist "!INSTALL_DIR!\bin\!BINARY_NAME!" (
    echo ERROR: !INSTALL_DIR!\bin\!BINARY_NAME! not found.
    exit /b 1
)

REM ---- First-run setup ----
if not exist "!USER_HOME!" (
    echo ==^> First run: creating working directory at !USER_HOME!
    mkdir "!USER_HOME!"
)

REM Junction read-only assets (skip if already exists)
for %%A in (dashboard workflow-editor scripts doc) do (
    if not exist "!USER_HOME!\%%A" (
        if exist "!INSTALL_DIR!\%%A" (
            mklink /J "!USER_HOME!\%%A" "!INSTALL_DIR!\%%A" >nul 2>&1
        )
    )
)

REM Writable directories
if not exist "!USER_HOME!\queue" mkdir "!USER_HOME!\queue"
if not exist "!USER_HOME!\log" mkdir "!USER_HOME!\log"
if not exist "!USER_HOME!\workflows" mkdir "!USER_HOME!\workflows"

REM Copy example workflows on first run (only if workflows dir is empty)
if exist "!INSTALL_DIR!\workflows" (
    dir /b "!USER_HOME!\workflows\*" >nul 2>&1
    if errorlevel 1 (
        xcopy /s /q /y "!INSTALL_DIR!\workflows\*" "!USER_HOME!\workflows\" >nul 2>&1
        echo ==^> Copied example workflows to !USER_HOME!\workflows\
    )
)

REM Copy example config on first run
if not exist "!USER_HOME!\config.json" (
    if exist "!INSTALL_DIR!\config.json.example" (
        copy "!INSTALL_DIR!\config.json.example" "!USER_HOME!\config.json" >nul
        echo ==^> Created !USER_HOME!\config.json from example
        echo     Please edit it to set your API keys.
    )
)

REM ---- Python venv (first-run) ----
if not exist "!USER_HOME!\.venv" (
    echo ==^> Creating Python virtual environment at !USER_HOME!\.venv ...
    python -m venv "!USER_HOME!\.venv" 2>nul
    if exist "!USER_HOME!\.venv\Scripts\pip.exe" (
        "!USER_HOME!\.venv\Scripts\pip" install --quiet --upgrade pip 2>nul
        echo ==^> Installing Python tools (markitdown^) ...
        "!USER_HOME!\.venv\Scripts\pip" install --quiet "markitdown[all]" 2>nul
    ) else (
        echo WARNING: Could not create Python venv (python may not be on PATH^)
        echo          Shell tasks using markitdown will not work until fixed.
    )
)

REM Activate venv if present
if exist "!USER_HOME!\.venv\Scripts\activate.bat" (
    call "!USER_HOME!\.venv\Scripts\activate.bat"
)

REM ---- Open dashboard in default browser ----
if "!OPEN_BROWSER!"=="1" (
    start "" "http://localhost:8080" 2>nul
)

REM ---- Launch ----
echo ==^> Starting JarvisAgent (!EDITION_LABEL! edition) in !USER_HOME!
echo     Dashboard: http://localhost:8080
if /i "!EDITION_LABEL!"=="Studio" echo     Editor:    http://localhost:8080/editor
echo.
cd /d "!USER_HOME!"
"!INSTALL_DIR!\bin\!BINARY_NAME!"
'@
Set-Content -Path "$StageDir\jarvisagent.bat" -Value $launcherContent -Encoding ASCII
# Edition-specific launcher copies (same script, edition detected from filename)
Copy-Item "$StageDir\jarvisagent.bat" "$StageDir\jarvisagent-engine.bat"
Copy-Item "$StageDir\jarvisagent.bat" "$StageDir\jarvisagent-studio.bat"
Copy-Item "$StageDir\jarvisagent.bat" "$StageDir\j9t.bat"

# Setup-venv helper script (standalone, for manual venv recreation)
$venvContent = @'
@echo off
setlocal enabledelayedexpansion
REM Creates a Python virtual environment in %USERPROFILE%\JarvisAgent\.venv
REM Normally the launcher handles this automatically on first run.
REM Use this script to recreate or repair the venv.

set "TARGET=%USERPROFILE%\JarvisAgent"
if not "%~1"=="" set "TARGET=%~1"

echo ==> Creating Python virtual environment in !TARGET!\.venv ...
python -m venv "!TARGET!\.venv"
"!TARGET!\.venv\Scripts\pip" install --quiet --upgrade pip

echo ==> Installing Python tools (markitdown) ...
"!TARGET!\.venv\Scripts\pip" install --quiet "markitdown[all]"

echo.
echo ==> Python venv created at !TARGET!\.venv\
echo.
echo NOTE: PDF workflows (vehicleTroubleshootingGuide) also require:
echo   mmdc:   npm install -g @mermaid-js/mermaid-cli@10.x
echo   pandoc: choco install pandoc miktex
echo.
pause
'@
Set-Content -Path "$StageDir\setup-venv.bat" -Value $venvContent -Encoding ASCII

# ---- Create .zip ----
Write-Host "==> Creating .zip package ..."
$zipPath = "$BuildDir\$PkgName.zip"
Compress-Archive -Path "$StageDir\*" -DestinationPath $zipPath -Force

$zipSize = (Get-Item $zipPath).Length / 1MB
Write-Host ""
Write-Host "==> Package created: $zipPath"
Write-Host ("    Size: {0:N1} MB" -f $zipSize)
Write-Host ""
Write-Host "    Extract and run:"
Write-Host "      1. Unzip $PkgName.zip"
Write-Host "      2. Run jarvisagent.bat"
Write-Host ""
Write-Host "    On first launch, the launcher will:"
Write-Host "      - Create %USERPROFILE%\JarvisAgent with your working data"
Write-Host "      - Set up a Python virtual environment"
Write-Host "      - Copy example config and workflows"
Write-Host "      - Open the dashboard in your browser"
Write-Host ""
Write-Host "    Options:"
Write-Host "      jarvisagent --home C:\path\to\dir   custom working directory"
Write-Host "      jarvisagent --no-browser             skip browser launch"
