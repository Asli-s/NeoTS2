# Crashes neots2 on purpose in every way its crash handler catches and checks that each run left a
# report: a crash folder with crash.txt naming the crash and a minidump. No dialogs are shown.
#   powershell -File tools\crashcheck.ps1 [-Exe build\Release\neots2.exe]
param([string]$Exe = "build\Release\neots2.exe")

$kinds = [ordered]@{
    access    = "access violation"
    stack     = "stack overflow"
    abort     = "abort"
    terminate = "std::terminate"
    throw     = "C++ exception not caught"
    purecall  = "pure virtual call"
    parameter = "invalid parameter"
    thread    = "access violation"
    twice     = "access violation"
}
$root = Join-Path ([IO.Path]::GetTempPath()) ("neots2-crashcheck-" + [Guid]::NewGuid().ToString("N"))
$failed = 0
foreach ($kind in $kinds.Keys) {
    $folder = Join-Path $root $kind
    $p = Start-Process -FilePath $Exe -ArgumentList "--log-folder", "`"$folder`"", "--no-crash-dialog", "--crash-test", $kind -PassThru -Wait -WindowStyle Hidden
    $report = Get-ChildItem $folder -Directory -Filter "crash-*" -ErrorAction SilentlyContinue | Select-Object -First 1
    $what = if ($report) { (Select-String -Path (Join-Path $report.FullName "crash.txt") -Pattern "^What:" | Select-Object -First 1).Line } else { "" }
    $dump = $report -and (Test-Path (Join-Path $report.FullName "neots2.dmp"))
    $ok = $what -and $what.Contains($kinds[$kind]) -and $dump
    if (-not $ok) { $failed++ }
    "{0} {1,-10} exit 0x{2:X8}  {3}" -f ($(if ($ok) { "[pass]" } else { "[FAIL]" })), $kind, $p.ExitCode, $what
}
# A failure Windows ends at once can't be reported by the program: the next start finds it.
$folder = Join-Path $root "fastfail"
$p = Start-Process -FilePath $Exe -ArgumentList "--log-folder", "`"$folder`"", "--no-crash-dialog", "--crash-test", "fastfail" -PassThru -Wait -WindowStyle Hidden
Start-Sleep -Seconds 3  # Windows notes the crash in its event log just after
Start-Process -FilePath $Exe -ArgumentList "--log-folder", "`"$folder`"", "--no-crash-dialog" -Wait -WindowStyle Hidden
$report = Get-ChildItem $folder -Directory -Filter "crash-*-unexpected*" -ErrorAction SilentlyContinue | Select-Object -First 1
$text = if ($report) { Get-Content (Join-Path $report.FullName "crash.txt") -Raw } else { "" }
$ok = $text.Contains("NeoTS2 closed unexpectedly") -and $text.Contains("crashing on purpose: fastfail")
if (-not $ok) { $failed++ }
$what = if ($text -match "What \(as Windows noted it\): (.*)") { $Matches[1] } else { "Windows noted nothing" }
"{0} {1,-10} exit 0x{2:X8}  next start: {3}" -f ($(if ($ok) { "[pass]" } else { "[FAIL]" })), "fastfail", $p.ExitCode, $what

Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
"`n$($kinds.Count + 1) crash kinds, $failed failed"
exit $failed
