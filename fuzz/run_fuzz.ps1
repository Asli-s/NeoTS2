param(
    [ValidateSet("qfs", "dbpf", "bhav", "str", "bcon", "glob", "objd", "objt", "objm", "xobj", "wallgraph", "array3d", "srel", "sdsc", "fami", "famt", "slot", "swaf", "propset", "ttab", "luascript", "ngbh", "datatables", "scenegraph", "stringmap", "walllayer", "namemap", "fencepostlayer", "roofs", "terrain", "reia", "anim", "image", "lot_description", "nhood_objects", "nhood_terrain", "objf", "reference_list", "simi", "camera_config", "nhood_paint_rules", "ui_script")] [string]$Target = "dbpf",
    [int]$Minutes = 10
)

$root = Split-Path $PSScriptRoot -Parent
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$env:PATH = "$($msvc.FullName)\bin\Hostx64\x64;$env:PATH"

& $cmake -S $root -B "$root\build-fuzz" -DNEOTS2_FUZZ=ON | Out-Null
& $cmake --build "$root\build-fuzz" --config RelWithDebInfo --target "${Target}_fuzz" -- /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

New-Item -ItemType Directory -Force "$root\fuzz\findings", "$root\fuzz\corpus\$Target" | Out-Null
& "$root\build-fuzz\RelWithDebInfo\${Target}_fuzz.exe" "$root\fuzz\corpus\$Target" `
    "-max_total_time=$($Minutes * 60)" -print_final_stats=1 `
    "-artifact_prefix=$root\fuzz\findings\$Target-"
