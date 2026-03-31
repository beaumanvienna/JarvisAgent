# @jarvis-script
# @short: Compile a single C++ source file with g++
# @params: Source — path to the .cpp source file, Output — path to the .o object file
# @description: Compiles a C++ source file using g++ with -Wall -Wextra -std=c++20.
#   Falls back to creating a placeholder file if g++ is not installed.
# @outputs: Output — the compiled object file
param(
    [string]$Source,
    [string]$Output
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Write-Output "[compile] $Source -> $Output"

if (Get-Command g++ -ErrorAction SilentlyContinue)
{
    & g++ -Wall -Wextra -std=c++20 -c $Source -o $Output
}
else
{
    Write-Output "[compile] g++ not found; creating replacement file"
    "g++ not found; replacement object file for $(Split-Path $Source -Leaf)" | Set-Content $Output
}
