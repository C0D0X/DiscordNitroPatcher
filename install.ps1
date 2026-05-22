# install.ps1 — friend-side installer. Copies dnp.exe to %LOCALAPPDATA%\dnp\ then runs --install.
# Run with:    powershell -ExecutionPolicy Bypass -File install.ps1
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $here 'dnp.exe'

if (-not (Test-Path $src)) {
    Write-Error "dnp.exe not found next to install.ps1 (looked at $src)"
    exit 1
}

$dstDir = Join-Path $env:LOCALAPPDATA 'dnp'
New-Item -ItemType Directory -Force -Path $dstDir | Out-Null

$dst = Join-Path $dstDir 'dnp.exe'
Copy-Item -Force $src $dst

# Strip mark-of-the-web so SmartScreen doesn't re-prompt every launch.
try { Unblock-File -Path $dst } catch {}
try { Remove-Item -Path "$($dst):Zone.Identifier" -ErrorAction SilentlyContinue } catch {}

# Run installer.
& $dst --install
$rc = $LASTEXITCODE
if ($rc -ne 0) {
    Write-Warning "dnp --install exited with code $rc"
    exit $rc
}
Write-Host "Installed at $dst. Discord patch applied. Background daemon registered."
