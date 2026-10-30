# Shows whether a change left the engine's results as they were: the tests, a simulation of each
# lot, and each lot's poses as numbers (its Sims' places, clips and bones and its moving objects,
# once a second for 30 seconds). With -Shots it adds pictures of each lot from three views and of
# each neighborhood picked by its number in the main menu, for changes to how things are drawn.
# Save a baseline before the change, then check:
#   powershell -File tools\regression.ps1 -Save -Install <game folder> -Lots <lot.package>[,...] [-Hoods 4[,...]] [-Shots]
#   powershell -File tools\regression.ps1
#   powershell -File tools\regression.ps1 -Save    (a new baseline with the same game folder, lots and neighborhoods)
# The check reruns everything with the same game folder and lots and lists what differs. It ends
# with exit code 0 when nothing does. A simulation or picture that is meant to change needs a new
# baseline (-Save again). A check takes pictures only when its baseline has them. Poses that moved
# 2 mm or less count as the same. Lots run with their random draws fixed (--seed). Runs need the
# game's files, so this is a local check, not part of CI.
# It builds vm_tests, simrun and neots2 in -Build first (-NoBuild skips that). Neighborhoods are shot
# with their ground's texture variants fixed (--seed), which the game otherwise picks anew on every load.
param(
    [switch]$Save,
    [string]$Install = "",
    [string[]]$Lots = @(),
    [string[]]$Hoods = @(),
    [string]$Build = "build\Release",
    [string]$Baseline = "build\regression",
    [int]$Ticks = 14400,
    [switch]$Shots,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
# Lists given through powershell -File arrive as one "a,b,c" string.
$Lots = @($Lots | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Hoods = @($Hoods | ForEach-Object { "$_" -split ',' } | Where-Object { $_ } | ForEach-Object { [int]$_ })
$root = Split-Path $PSScriptRoot -Parent
function Resolve-Here([string]$path) { if ([IO.Path]::IsPathRooted($path)) { $path } else { Join-Path $root $path } }
$Build = Resolve-Here $Build
$Baseline = Resolve-Here $Baseline
$settingsFile = Join-Path $Baseline "settings.json"

# A check, or a -Save without -Install, uses the game folder, lots and neighborhoods saved last time.
if (-not $Save -or -not $Install) {
    if (-not (Test-Path $settingsFile)) { throw "No baseline in $Baseline yet: run with -Save -Install <game folder> -Lots <lot.package> first." }
    $saved = Get-Content $settingsFile -Raw | ConvertFrom-Json
    $Install = $saved.install
    $Lots = @($saved.lots)
    $Hoods = @($saved.hoods | Where-Object { $_ -ne $null })
    $Ticks = $saved.ticks
    $Shots = [bool]$saved.shots
}
if ($Lots.Count -eq 0) { throw "No lots: pass -Lots <lot.package>." }
foreach ($file in @($Install) + $Lots) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Not found: $file" }
}
# The three programs are built first, so no result comes from an older build of one of them.
if (-not $NoBuild) {
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) { $cmake = & $vswhere -latest -find "**\CMake\bin\cmake.exe" | Select-Object -First 1 }
    }
    if (-not $cmake) { throw "cmake not found: build vm_tests, simrun and neots2 yourself and pass -NoBuild." }
    & $cmake --build (Split-Path $Build -Parent) --config (Split-Path $Build -Leaf) --target vm_tests simrun neots2 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "The build failed: run it yourself to see why." }
}
foreach ($exe in "vm_tests.exe", "simrun.exe", "neots2.exe") {
    if (-not (Test-Path (Join-Path $Build $exe))) { throw "No $exe in ${Build}: build it first." }
}

# Three views of every lot: the app's own first view, the far side with walls cut away and the Sims
# two seconds into their animations, and a far view with walls down and no roofs.
$views = [ordered]@{
    first   = @()
    cutaway = @("2.36", "0.7", "25", "--walls", "cutaway", "--time", "2")
    above   = @("-0.79", "0.7", "60", "--walls", "down", "--roofs", "off")
}

# Runs a program, waits for it, and returns its output and error text without the log's timestamps,
# which differ between runs.
function Invoke-Quiet([string]$exe, [string[]]$arguments, [string]$out) {
    $errFile = "$out.err"
    $start = @{ FilePath = $exe; NoNewWindow = $true; PassThru = $true; RedirectStandardOutput = $out; RedirectStandardError = $errFile }
    if ($arguments.Count -gt 0) {
        $start.ArgumentList = $arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    }
    $p = Start-Process @start
    $p.WaitForExit()
    $text = @(Get-Content -LiteralPath $out) + @(Get-Content -LiteralPath $errFile)
    Remove-Item -LiteralPath $errFile
    $text | ForEach-Object { $_ -replace '^\d\d:\d\d:\d\d\.\d+ ', '' } | Set-Content -Encoding utf8 -LiteralPath $out
    return $p.ExitCode
}

function Run-All([string]$folder) {
    if (Test-Path $folder) { Remove-Item -Recurse -Force $folder }
    New-Item -ItemType Directory -Force $folder | Out-Null
    $logs = Join-Path $folder "logs"

    # The tests: which ran and whether each passed.
    $tests = Join-Path $folder "tests.txt"
    Invoke-Quiet (Join-Path $Build "vm_tests.exe") @() $tests | Out-Null
    (Get-Content $tests | Where-Object { $_ -match '^\[(pass|FAIL)\] ' } | Sort-Object) | Set-Content -Encoding utf8 $tests

    for ($n = 0; $n -lt $Lots.Count; ++$n) {
        $lot = $Lots[$n]
        Invoke-Quiet (Join-Path $Build "simrun.exe") @($Install, "--lot", $lot, "$Ticks", "--on-screen", "--overlays", "--seed", "1") (Join-Path $folder "lot$n-simulation.txt") | Out-Null
        $numbers = Join-Path $folder "lot$n-numbers.txt"
        Invoke-Quiet (Join-Path $Build "neots2.exe") @($Install, $lot, "--numbers", $numbers, "--time", "30", "--seed", "1", "--no-crash-dialog", "--log-folder", $logs) (Join-Path $folder "lot$n-numbers.log") | Out-Null
        Remove-Item -LiteralPath (Join-Path $folder "lot$n-numbers.log")
        if (-not $Shots) {
            continue
        }
        foreach ($view in $views.Keys) {
            $shot = Join-Path $folder "lot$n-$view.png"
            $arguments = @($Install, $lot, "--shot", $shot, "--seed", "1") + $views[$view] + @("--no-crash-dialog", "--log-folder", $logs)
            Invoke-Quiet (Join-Path $Build "neots2.exe") $arguments (Join-Path $folder "lot$n-$view.txt") | Out-Null
            Remove-Item -LiteralPath (Join-Path $folder "lot$n-$view.txt")
        }
    }
    foreach ($hood in $(if ($Shots) { $Hoods } else { @() })) {
        $shot = Join-Path $folder "hood$hood.png"
        $arguments = @($Install, "--menu", "--play", "$hood", "--seed", "1", "--view", "-0.785", "0.9", "300", "--shot", $shot, "--no-crash-dialog", "--log-folder", $logs)
        Invoke-Quiet (Join-Path $Build "neots2.exe") $arguments (Join-Path $folder "hood$hood.txt") | Out-Null
        Remove-Item -LiteralPath (Join-Path $folder "hood$hood.txt")
    }
    if (Test-Path $logs) { Remove-Item -Recurse -Force $logs }
}

if ($Save) {
    Run-All $Baseline
    @{ install = $Install; lots = $Lots; hoods = $Hoods; ticks = $Ticks; shots = [bool]$Shots } | ConvertTo-Json | Set-Content -Encoding utf8 $settingsFile
    $count = (Get-ChildItem $Baseline -File | Where-Object Name -ne "settings.json").Count
    "Saved a baseline of $count results in $Baseline."
    return
}

# The lines of two pose number files that differ by more than 2 mm, as "line: was / now". Lines
# match when their words are the same and their numbers differ by no more than that.
function Compare-Numbers([string]$before, [string]$after) {
    $a = @(Get-Content -LiteralPath $before)
    $b = @(Get-Content -LiteralPath $after)
    $out = [System.Collections.Generic.List[string]]::new()
    if ($a.Count -ne $b.Count) { $out.Add("was $($a.Count) lines, now $($b.Count)") }
    for ($i = 0; $i -lt [Math]::Min($a.Count, $b.Count); ++$i) {
        if ($a[$i] -ceq $b[$i]) { continue }
        $x = $a[$i] -split ' '
        $y = $b[$i] -split ' '
        $same = $x.Count -eq $y.Count
        for ($j = 0; $same -and $j -lt $x.Count; ++$j) {
            if ($x[$j] -ceq $y[$j]) { continue }
            $u = 0.0
            $v = 0.0
            $same = [double]::TryParse($x[$j], [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$u) -and
                    [double]::TryParse($y[$j], [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$v) -and
                    [Math]::Abs($u - $v) -le 0.0021
        }
        if (-not $same) { $out.Add("$($i + 1): was $($a[$i].Trim()) / now $($b[$i].Trim())") }
    }
    return $out
}

$latest = Join-Path $Baseline "latest"
Run-All $latest
$different = 0
foreach ($before in Get-ChildItem $Baseline -File | Where-Object Name -ne "settings.json") {
    $after = Join-Path $latest $before.Name
    if (-not (Test-Path $after)) {
        "MISSING  $($before.Name)"
        ++$different
        continue
    }
    if ((Get-FileHash $before.FullName).Hash -eq (Get-FileHash $after).Hash) {
        "same     $($before.Name)"
        continue
    }
    if ($before.Name -like "*-numbers.txt") {
        $moved = @(Compare-Numbers $before.FullName $after)
        if ($moved.Count -eq 0) {
            "same     $($before.Name) (within 2 mm)"
            continue
        }
        ++$different
        "CHANGED  $($before.Name): $($moved.Count) lines"
        $moved | Select-Object -First 10 | ForEach-Object { "           $_" }
        continue
    }
    ++$different
    "CHANGED  $($before.Name)"
    if ($before.Extension -eq ".txt") {
        Compare-Object (Get-Content $before.FullName) (Get-Content $after) | Select-Object -First 10 | ForEach-Object {
            "           $(if ($_.SideIndicator -eq '<=') { 'was' } else { 'now' }): $($_.InputObject)"
        }
    } else {
        "           compare $($before.FullName) with $after"
    }
}
if ($different -eq 0) {
    "Nothing changed."
} else {
    "$different results changed (the new ones are in $latest)."
}
exit $(if ($different -eq 0) { 0 } else { 1 })
