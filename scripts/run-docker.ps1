# run-docker.ps1 — Pull and run JarvisAgent via Docker (Windows PowerShell).
#
# Usage:
#   .\scripts\run-docker.ps1                    # uses ~/JarvisAgent as data directory
#   .\scripts\run-docker.ps1 -DataDir C:\path   # uses custom data directory

param(
    [string]$DataDir = "$env:USERPROFILE\JarvisAgent"
)

$ErrorActionPreference = "Stop"
$Image = "ghcr.io/beaumanvienna/jarvisagent:latest"

Write-Host "==> Data directory: $DataDir"
if (-not (Test-Path $DataDir)) {
    New-Item -ItemType Directory -Path $DataDir | Out-Null
}

Write-Host "==> Pulling $Image"
docker pull $Image

Write-Host "==> Starting JarvisAgent"
Write-Host "    Dashboard: http://localhost:8080"
Write-Host "    Editor:    http://localhost:8080/editor"
Write-Host ""

docker run -it --rm `
    -p 8080:8080 `
    -v "${DataDir}:/app" `
    $Image @args
