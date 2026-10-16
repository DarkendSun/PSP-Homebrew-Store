# Build EBOOT.PBP with the pspdev Docker image (needs Docker Desktop).
# Usage: powershell -File app\build.ps1 [-Relay http://psp-homebrew-relay.<you>.workers.dev]
param([string]$Relay = "http://psp-homebrew-relay.YOUR-SUBDOMAIN.workers.dev")
$ErrorActionPreference = "Stop"
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Error "Docker isn't installed. Install Docker Desktop (https://www.docker.com/products/docker-desktop/), or push to GitHub and let the Actions workflow build it."
}
$app = $PSScriptRoot
docker run --rm -v "${app}:/src" -w /src pspdev/pspdev:latest make "RELAY=$Relay"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Built $app\EBOOT.PBP"
