# @jarvis-script
# @short: Pull and run JarvisAgent via Docker on Windows PowerShell
# @params: [-DataDir <path>]
# @description: Windows PowerShell equivalent of run-docker.sh. Pulls the
#   jarvisagent container image, mounts a per-user data directory (default
#   ~/JarvisAgent, overridable with -DataDir), and starts the container with
#   the web UI exposed on localhost:8080/8443.
# @outputs: A running JarvisAgent container on Windows
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
docker pull $Image 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    $cached = docker image inspect $Image 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "WARNING: Pull failed (no internet?). Using cached image."
    } else {
        Write-Host "ERROR: Pull failed and no cached image found. Check your internet connection."
        exit 1
    }
}

Write-Host "==> Starting JarvisAgent"
Write-Host "    Dashboard: http://localhost:8080"
Write-Host "    Editor:    http://localhost:8080/editor"
Write-Host ""

# Check if port 8080 is already in use
$portCheck = netstat -ano 2>$null | Select-String ":8080\s"
if ($portCheck) {
    Write-Host "ERROR: Port 8080 is already in use. Stop the other process first."
    exit 1
}

docker run -it --rm `
    -p 8080:8080 `
    -v "${DataDir}:/app" `
    $Image @args
