# run-docker.ps1 — Pull and run JarvisAgent via Docker (Windows PowerShell).
#
# Usage:
#   .\scripts\run-docker.ps1                    # uses ~/JarvisAgent as data directory
#   .\scripts\run-docker.ps1 -DataDir C:\path   # uses custom data directory

param(
    [string]$DataDir = "$env:USERPROFILE\JarvisAgent"
)

$ErrorActionPreference = "Stop"

# ---- Pre-flight: check Docker is available ----
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: 'docker' not found. Please install Docker Desktop first:"
    Write-Host "  https://docs.docker.com/desktop/install/windows-install/"
    exit 1
}

try {
    docker info 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Docker not reachable" }
} catch {
    Write-Host "ERROR: Cannot connect to the Docker daemon."
    Write-Host ""
    Write-Host "  Make sure Docker Desktop is running."
    exit 1
}

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
