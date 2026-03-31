# @jarvis-script
# @short: Create a static library archive from object files
# @params: Obj1 — first object file path, Obj2 — second object file path, Archive — output archive path
# @description: Creates a static library using ar rcs. Falls back to a placeholder file if ar is not installed.
# @outputs: Archive — the created .a archive file
param(
    [string]$Obj1,
    [string]$Obj2,
    [string]$Archive
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Write-Output "[archive] $Obj1 $Obj2 -> $Archive"

if (Get-Command ar -ErrorAction SilentlyContinue)
{
    & ar rcs $Archive $Obj1 $Obj2
}
else
{
    Write-Output "[archive] ar not found; creating replacement file"
    $obj1Name = Split-Path $Obj1 -Leaf
    $obj2Name = Split-Path $Obj2 -Leaf
    "ar not found; replacement archive containing $obj1Name $obj2Name" | Set-Content $Archive
}
