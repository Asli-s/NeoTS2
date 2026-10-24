# Makes a NeoTS2 release from the main branch, every step in order, and stops at the first problem.
# Run it from the repository, in a normal console (it asks before pushing anything):
#   powershell -File tools\release.ps1 [-Next <x.y.z>] [-Tool <path to symbol-upload-windows.exe>]
#   powershell -File tools\release.ps1 -Rehearse    (all checks, build and tests, nothing uploaded, tagged or committed)
#
# 1. Checks main: the release number in CMakeLists.txt is new and larger than the last release, and
#    main matches its remote when there is one.
# 2. Builds main's commit in a fresh copy of the repository (so local changes can't get in), as a
#    release with warnings as errors, runs the tests and checks the version the exe carries.
# 3. Uploads the symbols to BugSplat (see upload_symbols.ps1) and keeps the exe and .pdb files in
#    releases\v<number>, with the zip players download (see package.ps1). A rehearsal makes the zip
#    too, in releases\rehearsal.
# 4. Tags the commit v<number>, and pushes the tag after asking, when there is a remote.
# 5. Raises the number in CMakeLists.txt to -Next (by default the next minor release) on main.
#
# The build settings (generator, SDL3_DIR, freetype_DIR) are taken from an existing build folder (-Settings).
param(
    [string]$Next = "",
    [string]$Tool = "symbol-upload-windows.exe",
    [string]$Settings = "build",
    [switch]$Rehearse
)

$ErrorActionPreference = "Stop"

# Programs are judged by their exit code only: Windows PowerShell turns anything they write to the
# error stream into an error once output is redirected, and the tests write warnings there.
function Invoke-Native([string]$exe, [string[]]$arguments) {
    $ErrorActionPreference = "Continue"
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe $($arguments -join ' ') failed ($LASTEXITCODE)" }
}

function Get-Git([string[]]$arguments) {
    $ErrorActionPreference = "Continue"
    $out = & git @arguments
    if ($LASTEXITCODE -ne 0) { throw "git $($arguments -join ' ') failed ($LASTEXITCODE)" }
    return "$out".Trim()
}

function Get-ReleaseNumber([string]$cmakeLists) {
    if ($cmakeLists -match 'project\(NeoTS2 VERSION (\d+\.\d+\.\d+)') { return $Matches[1] }
    throw "No 'project(NeoTS2 VERSION x.y.z' line in CMakeLists.txt"
}

$root = Get-Git @("rev-parse", "--show-toplevel")
Set-Location $root

# 1. Main and its number.
if ((Get-Git @("rev-parse", "--is-shallow-repository")) -eq "true") {
    throw "This copy is shallow: git fetch --unshallow first, so the tags and counts are complete."
}
$remote = (& git remote) | Select-Object -First 1
if ($remote) {
    Invoke-Native git @("fetch", "-q", "--tags", $remote)
    $theirs = Get-Git @("rev-parse", "$remote/main")
    if ((Get-Git @("rev-parse", "main")) -ne $theirs) {
        throw "main differs from $remote/main: push or pull first, so the release is the shared code."
    }
}
$commit = Get-Git @("rev-parse", "main")
$short = Get-Git @("rev-parse", "--short=10", "main")
$number = Get-ReleaseNumber ((& git show "main:CMakeLists.txt") -join "`n")
$tag = "v$number"
if (& git tag -l $tag) {
    throw "$tag already exists: raise the number in CMakeLists.txt on main first."
}
$released = @(& git tag -l "v*" | Where-Object { $_ -match '^v\d+\.\d+\.\d+$' } | ForEach-Object { [version]$_.Substring(1) })
$last = $released | Sort-Object | Select-Object -Last 1
if ($last -and [version]$number -le $last) {
    throw "$number is not larger than the last release, v$last."
}
if (-not $Next) {
    $v = [version]$number
    $Next = "$($v.Major).$($v.Minor + 1).0"
}
if ($Next -notmatch '^\d+\.\d+\.\d+$' -or [version]$Next -le [version]$number) {
    throw "-Next must be a release number larger than $number, not '$Next'."
}
if (-not $Rehearse) {
    if (-not $env:BUGSPLAT_CLIENT_ID -or -not $env:BUGSPLAT_CLIENT_SECRET) {
        throw "Set BUGSPLAT_CLIENT_ID and BUGSPLAT_CLIENT_SECRET first (see upload_symbols.ps1)."
    }
    if (-not (Get-Command $Tool -ErrorAction SilentlyContinue)) {
        throw "symbol-upload-windows.exe not found: pass -Tool <path> (see upload_symbols.ps1)."
    }
}
$keep = Join-Path $root "releases\$tag"
if (-not $Rehearse -and (Test-Path $keep)) {
    throw "$keep already exists."
}
$settingsFolder = if ([IO.Path]::IsPathRooted($Settings)) { $Settings } else { Join-Path $root $Settings }
$cache = Join-Path $settingsFolder "CMakeCache.txt"
if (-not (Test-Path $cache)) { throw "No $cache to take the build settings from: pass -Settings <build folder>." }
$cacheText = Get-Content $cache
$generator = ($cacheText | Where-Object { $_ -match '^CMAKE_GENERATOR:INTERNAL=(.+)$' } | Select-Object -First 1) -replace '^[^=]+=', ''
$platform = ($cacheText | Where-Object { $_ -match '^CMAKE_GENERATOR_PLATFORM:INTERNAL=(.+)$' } | Select-Object -First 1) -replace '^[^=]+=', ''
if (-not $generator) { throw "No generator in $cache." }
$options = @("-G", $generator)
if ($platform) { $options += @("-A", $platform) }
foreach ($name in "SDL3_DIR", "freetype_DIR") {
    $line = $cacheText | Where-Object { $_ -match "^${name}:[A-Z]+=(.+)$" } | Select-Object -First 1
    if (-not $line) { throw "$name is not set in $cache." }
    $options += "-D$name=$($line -replace "^${name}:[A-Z]+=", '')"
}
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { $cmake = & $vswhere -latest -find "**\CMake\bin\cmake.exe" | Select-Object -First 1 }
}
if (-not $cmake) { throw "cmake not found." }
"Releasing NeoTS2 $number from main ($short); the next number will be $Next."

# 2. A fresh copy of main's commit, built as the release and tested.
$copy = Join-Path ([IO.Path]::GetTempPath()) "neots2-release-$number"
if (Test-Path $copy) { Invoke-Native git @("worktree", "remove", "--force", $copy) }
Invoke-Native git @("worktree", "add", "-q", "--detach", $copy, $commit)
try {
    $build = Join-Path $copy "build-release"
    Invoke-Native $cmake (@("-S", $copy, "-B", $build, "-DNEOTS2_RELEASE=ON", "-DNEOTS2_STRICT=ON") + $options)
    Invoke-Native $cmake @("--build", $build, "--config", "Release", "--target", "neots2", "neots2-launcher", "vm_tests")
    Invoke-Native (Join-Path $build "Release\vm_tests.exe") @()

    $expected = "$number+$short"
    $header = Get-Content (Join-Path $build "generated\neots2_version.hpp") -Raw
    if ($header -notmatch "NEOTS2_VERSION `"$([regex]::Escape($expected))`"") {
        throw "The build stamped another version than $expected."
    }
    $info = (Get-Item (Join-Path $build "Release\neots2.exe")).VersionInfo
    if ($info.ProductVersion -ne $expected -or $info.FileVersionRaw -ne [version]"$number.0") {
        throw "neots2.exe carries version $($info.ProductVersion) ($($info.FileVersionRaw)), not $expected."
    }
    "Built and tested NeoTS2 $expected."
    if ($Rehearse) {
        & (Join-Path $root "tools\package.ps1") -Build (Join-Path $build "Release") -Out (Join-Path $root "releases\rehearsal")
        "Rehearsal done: nothing was uploaded, tagged or committed."
        return
    }

    # 3. Symbols, and the files to keep.
    & (Join-Path $root "tools\upload_symbols.ps1") -Build (Join-Path $build "Release") -Tool $Tool
    New-Item -ItemType Directory -Force $keep | Out-Null
    Copy-Item (Join-Path $build "Release\neots2.exe"), (Join-Path $build "Release\neots2-launcher.exe"), (Join-Path $build "Release\*.pdb") $keep
    & (Join-Path $root "tools\package.ps1") -Build (Join-Path $build "Release") -Out $keep

    # 4. The tag.
    Invoke-Native git @("tag", "-a", $tag, "-m", "NeoTS2 $number", $commit)
    "Tagged $tag."
    if ($remote) {
        $answer = Read-Host "Push $tag to $remote now? (y/n)"
        if ($answer -eq "y") {
            Invoke-Native git @("push", $remote, $tag)
        } else {
            "Not pushed: run 'git push $remote $tag' when ready."
        }
    }

    # 5. The next number, committed in the copy and brought into main.
    $bump = "release-next-$Next"
    Invoke-Native git @("-C", $copy, "switch", "-q", "-c", $bump)
    $lists = Join-Path $copy "CMakeLists.txt"
    $text = [IO.File]::ReadAllText($lists)
    $raised = $text.Replace("project(NeoTS2 VERSION $number ", "project(NeoTS2 VERSION $Next ")
    if ($raised -eq $text) { throw "Couldn't raise the number in CMakeLists.txt: change it to $Next by hand and commit." }
    [IO.File]::WriteAllText($lists, $raised)
    Invoke-Native git @("-C", $copy, "commit", "-q", "-m", "Start $Next after releasing $number", "--", "CMakeLists.txt")
    $mainCheckout = (& git worktree list --porcelain | Select-String -Pattern '^worktree (.+)$' |
                     ForEach-Object { $_.Matches[0].Groups[1].Value } |
                     Where-Object { (& git -C $_ symbolic-ref -q --short HEAD) -eq "main" } | Select-Object -First 1)
    if ($mainCheckout) {
        Invoke-Native git @("-C", $mainCheckout, "merge", "-q", "--ff-only", "--autostash", $bump)
    } else {
        Invoke-Native git @("branch", "-f", "main", $bump)
    }
    Invoke-Native git @("-C", $copy, "switch", "-q", "--detach")
    Invoke-Native git @("branch", "-q", "-D", $bump)
    "Released NeoTS2 $number ($tag); main now builds $Next-dev. Keep $keep with the release."
} finally {
    Set-Location $root
    $ErrorActionPreference = "Continue"
    & git worktree remove --force $copy
}
