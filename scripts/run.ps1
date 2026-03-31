# @jarvis-script
# @short: Run an executable located under workflows/
# @params: TargetPath — path relative to the workflows/ directory
# @description: Locates and executes a binary under workflows/. Validates that
#   the target exists before running it.
param(
    [string]$TargetPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$LaunchDir = Split-Path -Parent $ScriptDir
$Target = Join-Path $LaunchDir "workflows" $TargetPath

if (-not (Test-Path $Target))
{
    Write-Error "Error: '$Target' not found."
    exit 1
}

& $Target
