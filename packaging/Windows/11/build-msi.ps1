# build-msi.ps1 — Build an MSI installer for JarvisAgent using WiX Toolset.
#
# Usage:
#   cd packaging\Windows\11
#   .\build-msi.ps1
#
# Prerequisites:
#   - Run build-zip.ps1 first to create the staging directory
#   - WiX v4: dotnet tool install --global wix
#   - Or WiX v3: install from https://wixtoolset.org/ (heat.exe + candle.exe + light.exe)
#
# This script:
#   1. Harvests component groups from the staging directory (dashboard, workflow-editor, scripts, etc.)
#   2. Compiles and links the MSI using WiX

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot  = (Resolve-Path "$ScriptDir\..\..\..").Path
$PkgVersion = (Select-String -Path "$RepoRoot\premake5.lua" -Pattern 'JARVIS_AGENT_VERSION' |
    ForEach-Object { $_.Line -replace '.*\\"([^"]*)\\".*','$1' } | Select-Object -First 1)
if (-not $PkgVersion) { Write-Host "ERROR: could not extract version from premake5.lua"; exit 1 }
$BuildDir  = "$ScriptDir\build"
$StageDir  = "$BuildDir\JarvisAgent-${PkgVersion}-x64"
$MsiOutput = "$BuildDir\JarvisAgent-${PkgVersion}-x64.msi"

if (-not (Test-Path $StageDir)) {
    Write-Host "ERROR: Staging directory not found at $StageDir"
    Write-Host "       Run build-zip.ps1 first to create the package tree."
    exit 1
}

# ---- Inject version into WiX source ----
$WxsSource = "$ScriptDir\jarvisagent.wxs"
$WxsBuild  = "$BuildDir\jarvisagent.wxs"
Copy-Item $WxsSource $WxsBuild
(Get-Content $WxsBuild) -replace 'Version="0\.1\.0\.0"', "Version=`"$PkgVersion.0.0`"" `
    -replace 'JarvisAgent-x64', "JarvisAgent-${PkgVersion}-x64" | Set-Content $WxsBuild

# ---- Check for WiX ----
$wixV4 = Get-Command "wix" -ErrorAction SilentlyContinue
$heatCmd = Get-Command "heat" -ErrorAction SilentlyContinue

if ($wixV4) {
    Write-Host "==> Using WiX v4"

    Write-Host "==> Harvesting component groups ..."
    wix heat dir "$StageDir\dashboard" -cg DashboardFiles -dr DashboardDir -srd -ag -sfrag -o "$BuildDir\dashboard.wxs"
    wix heat dir "$StageDir\workflow-editor" -cg WorkflowEditorFiles -dr WorkflowEditorDir -srd -ag -sfrag -o "$BuildDir\workflow-editor.wxs"
    wix heat dir "$StageDir\scripts" -cg ScriptFiles -dr ScriptsDir -srd -ag -sfrag -o "$BuildDir\scripts.wxs"
    wix heat dir "$StageDir\workflows" -cg WorkflowFiles -dr WorkflowsDir -srd -ag -sfrag -o "$BuildDir\workflows.wxs"
    wix heat dir "$StageDir\doc" -cg DocFiles -dr DocDir -srd -ag -sfrag -o "$BuildDir\doc.wxs"

    Write-Host "==> Building MSI ..."
    wix build `
        -o $MsiOutput `
        "$WxsBuild" `
        "$BuildDir\dashboard.wxs" `
        "$BuildDir\workflow-editor.wxs" `
        "$BuildDir\scripts.wxs" `
        "$BuildDir\workflows.wxs" `
        "$BuildDir\doc.wxs"

} elseif ($heatCmd) {
    Write-Host "==> Using WiX v3"

    Write-Host "==> Harvesting component groups ..."
    heat dir "$StageDir\dashboard"        -cg DashboardFiles       -dr DashboardDir       -srd -ag -sfrag -var var.DashboardSource       -o "$BuildDir\dashboard.wxs"
    heat dir "$StageDir\workflow-editor"  -cg WorkflowEditorFiles  -dr WorkflowEditorDir  -srd -ag -sfrag -var var.WorkflowEditorSource  -o "$BuildDir\workflow-editor.wxs"
    heat dir "$StageDir\scripts"          -cg ScriptFiles          -dr ScriptsDir          -srd -ag -sfrag -var var.ScriptsSource          -o "$BuildDir\scripts.wxs"
    heat dir "$StageDir\workflows"        -cg WorkflowFiles        -dr WorkflowsDir        -srd -ag -sfrag -var var.WorkflowsSource        -o "$BuildDir\workflows.wxs"
    heat dir "$StageDir\doc"              -cg DocFiles             -dr DocDir              -srd -ag -sfrag -var var.DocSource              -o "$BuildDir\doc.wxs"

    Write-Host "==> Compiling .wxs files ..."
    candle -arch x64 -o "$BuildDir\jarvisagent.wixobj"      "$WxsBuild"
    candle -arch x64 "-dvar.DashboardSource=$StageDir\dashboard"             -o "$BuildDir\dashboard.wixobj"       "$BuildDir\dashboard.wxs"
    candle -arch x64 "-dvar.WorkflowEditorSource=$StageDir\workflow-editor"  -o "$BuildDir\workflow-editor.wixobj"  "$BuildDir\workflow-editor.wxs"
    candle -arch x64 "-dvar.ScriptsSource=$StageDir\scripts"                 -o "$BuildDir\scripts.wixobj"          "$BuildDir\scripts.wxs"
    candle -arch x64 "-dvar.WorkflowsSource=$StageDir\workflows"             -o "$BuildDir\workflows.wixobj"        "$BuildDir\workflows.wxs"
    candle -arch x64 "-dvar.DocSource=$StageDir\doc"                         -o "$BuildDir\doc.wixobj"              "$BuildDir\doc.wxs"

    Write-Host "==> Linking MSI ..."
    $wixobjs = Get-ChildItem "$BuildDir\*.wixobj" | ForEach-Object { $_.FullName }
    light -o $MsiOutput $wixobjs

} else {
    Write-Host "ERROR: WiX Toolset not found."
    Write-Host "  Install WiX v4: dotnet tool install --global wix"
    Write-Host "  Or WiX v3:      https://wixtoolset.org/"
    Write-Host ""
    Write-Host "  The staging directory is ready at: $StageDir"
    Write-Host "  You can also use the portable .zip from build-zip.ps1"
    exit 1
}

$msiSize = (Get-Item $MsiOutput).Length / 1MB
Write-Host ""
Write-Host "==> MSI created: $MsiOutput"
Write-Host ("    Size: {0:N1} MB" -f $msiSize)
Write-Host ""
Write-Host "    Install: double-click $MsiOutput or run:"
Write-Host "      msiexec /i $MsiOutput"
Write-Host ""
Write-Host "    After install:"
Write-Host "      1. copy config.json.example config.json"
Write-Host "      2. Edit config.json (set API keys)"
Write-Host "      3. Run setup-venv.bat (one-time)"
Write-Host "      4. Run jarvisagent.bat"
