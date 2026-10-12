# Uploads a release build's symbols to BugSplat, so crash reports sent from that build show
# function names and lines. Run it for every release, from the repository root:
#   powershell -File tools\upload_symbols.ps1 [-Build build\Release] [-Tool <path to symbol-upload-windows.exe>]
# Needs BugSplat's symbol-upload tool and an OAuth client with the "symbols" scope, whose id and
# secret are read from the environment variables BUGSPLAT_CLIENT_ID and BUGSPLAT_CLIENT_SECRET.
param([string]$Build = "build\Release", [string]$Tool = "symbol-upload-windows.exe")

$ErrorActionPreference = "Stop"
if (-not $env:BUGSPLAT_CLIENT_ID -or -not $env:BUGSPLAT_CLIENT_SECRET) {
    throw "Set BUGSPLAT_CLIENT_ID and BUGSPLAT_CLIENT_SECRET (an OAuth client with the symbols scope, from BugSplat's OAuth Integrations page)."
}
$toolPath = (Get-Command $Tool -ErrorAction SilentlyContinue).Source
if (-not $toolPath) {
    throw "symbol-upload-windows.exe not found: get it from BugSplat (see its 'Upload Symbols with Symbol-Upload' guide) and pass -Tool <path>."
}
$header = Join-Path (Split-Path $Build -Parent) "generated\neots2_version.hpp"
$version = if ((Get-Content $header -Raw) -match 'NEOTS2_VERSION "([^"]+)"') { $Matches[1] } else { throw "No version in $header; build first." }
if ($version.EndsWith("-modified")) {
    Write-Warning "This build has uncommitted changes ($version); release builds should come from a clean commit."
}
Push-Location $Build
try {
    & $toolPath -b NeoTS2 -a NeoTS2 -v $version -i $env:BUGSPLAT_CLIENT_ID -s $env:BUGSPLAT_CLIENT_SECRET -f "**/{neots2.exe,neots2.pdb}"
    if ($LASTEXITCODE -ne 0) { throw "symbol-upload failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
"Uploaded the symbols of NeoTS2 $version"
