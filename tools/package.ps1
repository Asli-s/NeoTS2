# Makes the zip players download: the launcher and the game, the patched SDL3.dll, the shaders, the
# Microsoft C++ runtime the programs need, the licenses and a short start-here note.
#   powershell -File tools\package.ps1 [-Build <folder with the Release programs>] [-Out <folder>]
#
# The programs are taken as built, so build both neots2 and neots2-launcher in Release first.
# The zip is named after the version neots2.exe carries, and its SHA-256 is written beside it.
param(
    [string]$Build = "build\Release",
    [string]$Out = "releases"
)

$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot -Parent
$from = if ([IO.Path]::IsPathRooted($Build)) { $Build } else { Join-Path $root $Build }
$outFolder = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }

foreach ($name in "neots2.exe", "neots2-launcher.exe", "SDL3.dll", "shaders") {
    if (-not (Test-Path (Join-Path $from $name))) { throw "$name is missing from ${from}: build neots2 and neots2-launcher first." }
}
$version = (Get-Item (Join-Path $from "neots2.exe")).VersionInfo.ProductVersion
if (-not $version) { throw "neots2.exe carries no version." }
$launcherVersion = (Get-Item (Join-Path $from "neots2-launcher.exe")).VersionInfo.ProductVersion
if ($launcherVersion -and $launcherVersion -ne $version) {
    throw "neots2-launcher.exe is $launcherVersion but neots2.exe is ${version}: build both from the same commit."
}

# The C++ runtime files of the Visual Studio that built the programs. Microsoft allows shipping them
# beside the programs, so players need no separate runtime install.
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -property installationPath } else { "C:\Program Files\Microsoft Visual Studio\2022\Community" }
$redist = Get-ChildItem (Join-Path $vs "VC\Redist\MSVC") -Directory -ErrorAction SilentlyContinue |
          Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } | Select-Object -Last 1
$crt = if ($redist) { Get-ChildItem (Join-Path $redist.FullName "x64") -Directory -Filter "Microsoft.VC*.CRT" | Select-Object -First 1 }
if (-not $crt) { throw "The Visual C++ runtime files weren't found under $vs\VC\Redist\MSVC." }

$name = "NeoTS2-$version"
$stage = Join-Path ([IO.Path]::GetTempPath()) "neots2-package\$name"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $from "neots2.exe"), (Join-Path $from "neots2-launcher.exe"), (Join-Path $from "SDL3.dll") $stage
Copy-Item -Recurse (Join-Path $from "shaders") (Join-Path $stage "shaders")
Copy-Item (Join-Path $crt.FullName "*.dll") $stage
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")

# Every library's license, as their licenses ask when they are shipped.
$stb = Get-Content (Join-Path $root "third_party\stb\stb_image.h")
$stbStart = ($stb | Select-String -SimpleMatch "This software is available under 2 licenses" | Select-Object -Last 1).LineNumber
$notices = @(
    @("SDL 3 (zlib License)", (Get-Content -Raw (Join-Path $root "third_party\licenses\SDL3-LICENSE.txt"))),
    @("FreeType (The FreeType Project License). Portions of this software are copyright (c) 2024 The FreeType Project (www.freetype.org). All rights reserved.",
      (Get-Content -Raw (Join-Path $root "third_party\licenses\FreeType-FTL.txt"))),
    @("Dear ImGui (MIT License)", (Get-Content -Raw (Join-Path $root "third_party\imgui\LICENSE.txt"))),
    @("Noto Sans (SIL Open Font License 1.1)", (Get-Content -Raw (Join-Path $root "third_party\fonts\OFL.txt"))),
    @("stb_image (MIT License or public domain)", (($stb | Select-Object -Skip ($stbStart - 1)) -join "`r`n")),
    @("Lua 5.0.3 (MIT License)", (Get-Content -Raw (Join-Path $root "third_party\lua-5.0.3\COPYRIGHT"))),
    @("Microsoft Visual C++ runtime", "msvcp140*.dll, vcruntime140*.dll, concrt140.dll and vccorlib140.dll are Microsoft's redistributable runtime files, shipped as Visual Studio's license allows.")
)
$text = "NeoTS2 uses these libraries, and ships them inside its programs or beside them.`r`n"
foreach ($notice in $notices) {
    $text += "`r`n" + ("=" * 78) + "`r`n" + $notice[0] + "`r`n" + ("=" * 78) + "`r`n`r`n" + $notice[1].Trim() + "`r`n"
}
[IO.File]::WriteAllText((Join-Path $stage "THIRD-PARTY-NOTICES.txt"), $text)

$start = @"
NeoTS2 $version

BACK UP YOUR SAVES FIRST. NeoTS2 is early, unfinished software. Copy your save folder (in
Documents\EA Games, for example The Sims 2 Ultimate Collection) somewhere safe first, or make a new
save folder for NeoTS2 with the launcher's "New save folder..." button.

You need your own copy of The Sims 2: the Ultimate Collection or the Legacy Collection.

To play, run neots2-launcher.exe. It finds your install and your save folders. Choose them and
press Play.

Windows may say "Windows protected your PC", because NeoTS2 isn't signed yet. Click "More info",
then "Run anyway".

The launcher's settings and NeoTS2's logs are kept in %LOCALAPPDATA%\NeoTS2. To remove NeoTS2,
delete this folder and that one. Your saves are never touched.

Licenses: LICENSE.txt (NeoTS2) and THIRD-PARTY-NOTICES.txt (the libraries it uses).
"@
[IO.File]::WriteAllText((Join-Path $stage "Start here.txt"), $start.Replace("`r`n", "`n").Replace("`n", "`r`n"))

New-Item -ItemType Directory -Force $outFolder | Out-Null
$zip = Join-Path $outFolder "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$zip.sha256", "$hash  $name.zip`n")
Remove-Item -Recurse -Force (Split-Path $stage)
"Packaged $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
"SHA-256 $hash"
