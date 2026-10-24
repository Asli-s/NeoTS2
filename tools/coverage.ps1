# Measures which lines of the engine the tests run: builds the tests with Clang's coverage
# instrumentation, runs them, and prints a summary per file (and writes an HTML report to
# build-coverage\report). Needs Visual Studio 2022 with its Clang tools (clang-cl, llvm-cov).
#   powershell -File tools\coverage.ps1
param([string]$Report = "build-coverage\report")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$llvm = "$vs\VC\Tools\Llvm\x64\bin"
if (-not (Test-Path "$llvm\clang-cl.exe")) { $llvm = "$env:ProgramFiles\LLVM\bin" }  # a separate LLVM install
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$build = Join-Path $root "build-coverage"

# Clang and Ninja inside Visual Studio's x64 build environment.
$steps = @(
    "cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DNEOTS2_COVERAGE=ON",
    "cmake --build `"$build`" --target vm_tests"
)
foreach ($step in $steps) {
    cmd /c "set `"PATH=${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;%PATH%`" && call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set `"PATH=$llvm;$cmake;$ninja;%PATH%`" && $step"
    if ($LASTEXITCODE -ne 0) { throw "failed: $step" }
}

# Run the tests, writing raw counts, then merge them and report on the engine's own code.
$raw = Join-Path $build "tests.profraw"
$data = Join-Path $build "tests.profdata"
$env:LLVM_PROFILE_FILE = $raw
& "$build\vm_tests.exe" | Select-String "tests,|FAILED|\[FAIL\]"  # the summary, and any check that failed
& "$llvm\llvm-profdata.exe" merge -sparse $raw -o $data
$ignore = "third_party|tests[\\/]|tools[\\/]|fuzz[\\/]"
& "$llvm\llvm-cov.exe" report "$build\vm_tests.exe" "-instr-profile=$data" "-ignore-filename-regex=$ignore"
& "$llvm\llvm-cov.exe" show "$build\vm_tests.exe" "-instr-profile=$data" "-ignore-filename-regex=$ignore" `
    -format=html "-output-dir=$(Join-Path $root $Report)" | Out-Null
"HTML report: $(Join-Path $root $Report)\index.html"
