# @jarvis-script
# @short: Pull and run JarvisAgent via Docker on Windows PowerShell
# @params: [-DataDir <path>] [-Tls]
# @description: Windows PowerShell equivalent of run-docker.sh. Pulls the
#   jarvisagent container image, mounts a per-user data directory (default
#   ~/JarvisAgent, overridable with -DataDir), and starts the container.
#   Default is plain HTTP on port 8080; pass -Tls to serve HTTPS on port 8443
#   (requires certs under <DataDir>\certs\ — see doc/jarvisagent.md DOCKER
#   section).
# @outputs: A running JarvisAgent container on Windows
#
# Usage:
#   .\scripts\run-docker.ps1                    # HTTP on 8080, default data dir
#   .\scripts\run-docker.ps1 -DataDir C:\path   # HTTP on 8080, custom data dir
#   .\scripts\run-docker.ps1 -Tls               # HTTPS on 8443, default data dir
#   .\scripts\run-docker.ps1 -Tls -DataDir ...  # HTTPS + custom data dir

param(
    [string]$DataDir = "$env:USERPROFILE\JarvisAgent",
    [switch]$Tls
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

if ($Tls) {
    $Port   = 8443
    $Scheme = "https"
} else {
    $Port   = 8080
    $Scheme = "http"
}

Write-Host "==> Data directory: $DataDir"
if (-not (Test-Path $DataDir)) {
    New-Item -ItemType Directory -Path $DataDir | Out-Null
}

# ---- TLS pre-flight: verify cert files are present on the host ----
if ($Tls) {
    $Cert = Join-Path $DataDir "certs\j9t-cert.pem"
    $Key  = Join-Path $DataDir "certs\j9t-key.pem"
    if (-not (Test-Path $Cert) -or -not (Test-Path $Key)) {
        Write-Host ""
        Write-Host "ERROR: -Tls requires TLS certificate files in the data directory."
        Write-Host "Expected paths:"
        Write-Host "    $Cert"
        Write-Host "    $Key"
        Write-Host ""
        Write-Host "Generate a self-signed pair (OpenSSL required):"
        Write-Host "    mkdir ""$DataDir\certs"""
        Write-Host "    openssl req -x509 -newkey rsa:2048 -nodes ``"
        Write-Host "      -keyout ""$Key"" ``"
        Write-Host "      -out    ""$Cert"" ``"
        Write-Host "      -days 365 ``"
        Write-Host "      -subj ""/CN=localhost"" ``"
        Write-Host "      -addext ""subjectAltName=DNS:localhost,IP:127.0.0.1"""
        Write-Host ""
        Write-Host "For more detail see the DOCKER section of the user manual"
        Write-Host "(doc/jarvisagent.md) and certs/README.md."
        exit 1
    }
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
Write-Host "    Dashboard: ${Scheme}://localhost:${Port}"
Write-Host "    Editor:    ${Scheme}://localhost:${Port}/editor"
Write-Host ""

# Check if target host port is already in use
$portCheck = netstat -ano 2>$null | Select-String ":$Port\s"
if ($portCheck) {
    Write-Host "ERROR: Port $Port is already in use. Stop the other process first."
    exit 1
}

$DockerArgs = @("-it", "--rm", "-p", "${Port}:${Port}", "-v", "${DataDir}:/app")
if ($Tls) {
    $DockerArgs += @("-e", "J9T_TLS=1")
}
docker run @DockerArgs $Image @args
