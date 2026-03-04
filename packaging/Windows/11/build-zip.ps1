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
    Write-Host "==> Generating Visual Studio solution ..."
    Push-Location $RepoRoot
    premake5 vs2022
    Pop-Location

    Write-Host "==> Building C++ Release binary ..."
    $slnFiles = Get-ChildItem -Path $RepoRoot -Recurse -Filter *.sln
    foreach ($sln in $slnFiles) {
        Write-Host "    Building: $($sln.FullName)"
        msbuild "$($sln.FullName)" /m /p:Configuration=Release /p:Platform=x64
    }

    Write-Host "==> Building React dashboard ..."
    Push-Location "$RepoRoot\dashboard\ui"
    npm install
    npm run build
    Pop-Location

    Write-Host "==> Building React workflow editor ..."
    Push-Location "$RepoRoot\workflow-editor\ui"
    npm install
    npm run build
    Pop-Location
}

# ---- Assemble package tree ----
Write-Host "==> Assembling package tree ..."

# Binary
$binSrc = "$RepoRoot\bin\Release\jarvisAgent.exe"
New-Item -ItemType Directory -Force -Path "$StageDir\bin" | Out-Null
if (Test-Path $binSrc) {
    Copy-Item $binSrc "$StageDir\bin\jarvisAgent.exe"
} else {
    Write-Host "WARNING: bin\Release\jarvisAgent.exe not found (expected in dry-run)"
}

# React UIs
if (Test-Path "$RepoRoot\dashboard\ui\dist") {
    New-Item -ItemType Directory -Force -Path "$StageDir\dashboard\ui" | Out-Null
    Copy-Item -Recurse "$RepoRoot\dashboard\ui\dist" "$StageDir\dashboard\ui\dist"
} else {
    Write-Host "WARNING: dashboard\ui\dist not found (expected in dry-run)"
}

if (Test-Path "$RepoRoot\workflow-editor\ui\dist") {
    New-Item -ItemType Directory -Force -Path "$StageDir\workflow-editor\ui" | Out-Null
    Copy-Item -Recurse "$RepoRoot\workflow-editor\ui\dist" "$StageDir\workflow-editor\ui\dist"
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
    "aiCarMaintenancePipeline", "aiZipDemo", "exampleMakefile4",
    "make-example", "portfolioDividendAnalysis",
    "vehicleTroubleshootingGuide"
)
foreach ($jcwf in $jcwfFiles) {
    $src = "$RepoRoot\example\workflows\$jcwf.jcwf"
    if (Test-Path $src) { Copy-Item $src "$StageDir\workflows\" }
}
# Loose input files needed by the example workflows
$looseFiles = @(
    "app.cpp", "lib1.cpp", "lib2.cpp", "main.cpp", "mylib.h",
    "message_engine_question.txt", "message_tire_question.txt",
    "message_unclear_question.txt", "port62pos.csv"
)
foreach ($f in $looseFiles) {
    $src = "$RepoRoot\example\workflows\$f"
    if (Test-Path $src) { Copy-Item $src "$StageDir\workflows\" }
}
# message.txt — copy as regular file on Windows (no symlinks)
Copy-Item "$RepoRoot\example\workflows\message_engine_question.txt" "$StageDir\workflows\message.txt"

# Example config
Copy-Item "$RepoRoot\config.json" "$StageDir\config.json.example"

# Runtime directories
New-Item -ItemType Directory -Force -Path "$StageDir\queue" | Out-Null
New-Item -ItemType Directory -Force -Path "$StageDir\log" | Out-Null

# Documentation
New-Item -ItemType Directory -Force -Path "$StageDir\doc" | Out-Null
Copy-Item "$RepoRoot\README.md" "$StageDir\doc\README.md"
if (Test-Path "$RepoRoot\doc\JC_Workflow_Specification.md") {
    Copy-Item "$RepoRoot\doc\JC_Workflow_Specification.md" "$StageDir\doc\JC_Workflow_Specification.md"
}

# Launcher batch file
$launcherContent = @'
@echo off
REM JarvisAgent launcher for Windows
REM Requires bash on PATH (Git Bash or MSYS2) for shell tasks.

cd /d "%~dp0"

if "%~1"=="--help" ( bin\jarvisAgent.exe %* & exit /b )
if "%~1"=="-h" ( bin\jarvisAgent.exe %* & exit /b )
if "%~1"=="--version" ( bin\jarvisAgent.exe %* & exit /b )
if "%~1"=="-v" ( bin\jarvisAgent.exe %* & exit /b )

if not exist config.json (
    echo No config.json found in %~dp0
    echo Copy the example and edit it:
    echo   copy config.json.example config.json
    exit /b 1
)

bin\jarvisAgent.exe %*
'@
Set-Content -Path "$StageDir\jarvisagent.bat" -Value $launcherContent -Encoding ASCII

# Setup-venv helper script
$venvContent = @'
@echo off
REM Creates a Python virtual environment and installs tools.
REM Run this once after extracting the zip.

echo ==> Creating Python virtual environment in .venv ...
python -m venv .venv
.venv\Scripts\pip install --quiet --upgrade pip

echo ==> Installing Python tools (markitdown, md2pdf-mermaid, playwright) ...
.venv\Scripts\pip install --quiet "markitdown[all]" md2pdf-mermaid playwright

echo ==> Installing Playwright Chromium ...
.venv\Scripts\playwright install chromium

echo.
echo ==> Python venv created at .venv\
echo     Activate before running JarvisAgent:
echo       .venv\Scripts\activate
echo       jarvisagent.bat
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
Write-Host "      2. copy config.json.example config.json"
Write-Host "      3. Edit config.json (set API keys)"
Write-Host "      4. Run setup-venv.bat (one-time)"
Write-Host "      5. Run jarvisagent.bat"
