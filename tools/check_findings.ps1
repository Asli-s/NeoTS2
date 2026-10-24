# Compares a code-analysis log (MSVC /analyze build output or clang-tidy output) with the findings
# already reviewed in tools/known_findings.txt, and fails on any finding not listed there:
#   powershell -File tools\check_findings.ps1 -Log <build or clang-tidy output>
# A finding is its file, its warning (code or check) and its message, without line numbers, so code
# moving within a file doesn't count as new. Review a new finding, then fix it or add it to the list.
param(
    [Parameter(Mandatory = $true)][string]$Log,
    [string]$Known = ""
)

$ErrorActionPreference = "Stop"
if (-not $Known) { $Known = Join-Path $PSScriptRoot "known_findings.txt" }
$root = (Split-Path $PSScriptRoot -Parent).TrimEnd('\', '/')

function Get-Relative([string]$file) {
    $file = $file -replace '/', '\'
    $at = $file.IndexOf('\src\'), $file.IndexOf('\app\'), $file.IndexOf('\tests\'), $file.IndexOf('\tools\') | Where-Object { $_ -ge 0 } |
          Sort-Object | Select-Object -First 1
    if ($null -ne $at) { return $file.Substring($at + 1) }
    return $file
}

$findings = New-Object System.Collections.Generic.SortedSet[string]
foreach ($line in Get-Content -LiteralPath $Log) {
    # Libraries kept as they come (stb_image, Lua) are not reviewed here.
    if ($line -match '^\s*\S*[\\/]third_party[\\/]') {
        continue
    }
    # MSVC:<file>(<line>): warning C6011: <message> : Lines: ... [<project>]
    if ($line -match '^\s*(.+?)\((\d+)(,\d+)?\): warning (C\d+): (.+?)( \[[^\]]*\])?$') {
        # Stack sizes and line numbers in a message change with edits: they are left out.
        $message = ($Matches[5] -replace '\s*:\s*Lines:.*$', '' -replace "'\d+' bytes of stack", "'N' bytes of stack" -replace 'See line \d+', 'See line N').Trim()
        $findings.Add("$(Get-Relative $Matches[1]) | $($Matches[4]) | $message") | Out-Null
    }
    # clang-tidy: <file>:<line>:<column>: warning: <message> [<check>]
    elseif ($line -match '^(.+?):(\d+):(\d+): warning: (.+) \[([\w.,-]+)\]$') {
        $findings.Add("$(Get-Relative $Matches[1]) | $($Matches[5]) | $($Matches[4].Trim())") | Out-Null
    }
}
$listed = @(Get-Content -LiteralPath $Known | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object { $_.Trim() })
$new = @($findings | Where-Object { $listed -notcontains $_ })
$gone = @($listed | Where-Object { -not $findings.Contains($_) })
"$($findings.Count) findings, $($new.Count) new."
$new | ForEach-Object { "NEW   $_" }
# A listed finding no longer seen is only reported: the log may cover fewer files than the list.
$gone | ForEach-Object { "gone  $_" }
exit $(if ($new.Count -eq 0) { 0 } else { 1 })
