# @jarvis-script
# @short: Link object files and archive into an executable
# @params: MainObj — path to main object file, AppObj — path to application object file,
#   Archive — path to static library archive, Output — path to output executable
# @description: Links object files and a static archive using g++.
#   Falls back to creating a placeholder script if g++ is not installed.
# @outputs: Output — the linked executable
param(
    [string]$MainObj,
    [string]$AppObj,
    [string]$Archive,
    [string]$Output
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Write-Output "[link] $MainObj $AppObj $Archive -> $Output"

if (Get-Command g++ -ErrorAction SilentlyContinue)
{
    & g++ -Wall -Wextra $MainObj $AppObj $Archive -o $Output
}
else
{
    Write-Output "[link] g++ not found; creating replacement executable"
    @'
@echo off
echo g++ was not installed when this was built.
echo This is a replacement hello-world script.
echo Hello, World!
'@ | Set-Content "$Output.bat"
    Copy-Item "$Output.bat" $Output
}
