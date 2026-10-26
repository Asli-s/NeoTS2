param(
    [ValidateSet("qfs", "dbpf", "cobject", "bhav", "str", "bcon", "fcns", "glob", "objd", "objt", "objm", "xobj", "wallgraph", "array3d", "srel", "sdsc", "fami", "famt", "slot", "swaf", "propset", "ttab", "luascript", "ngbh", "datatables", "scenegraph", "stringmap", "walllayer", "namemap", "fencepostlayer", "roofs", "terrain", "reia", "anim", "image", "lot_description", "nhood_objects", "nhood_terrain", "objf", "reference_list", "simi", "weather", "neighborhood_id", "camera_config", "effects", "hood_drift", "ground_decal", "nhood_paint_rules", "ui_script", "cursor", "floorlight", "walllight", "windowlight", "lightgrid", "wallruns", "lighting", "lamplights", "portalrecords", "roomsamples", "lotlighting", "lotlevels", "objectsun", "route_search", "lua_runtime", "vm", "steam_files", "launcher_settings")] [string]$Target = "dbpf",
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
# Inputs are a few KB, so one allocation past 512 MB or an input running past 10 s is a bug (a
# size or count taken from the data without a check), reported like a crash.
$options = @("-max_total_time=$($Minutes * 60)", "-print_final_stats=1", "-malloc_limit_mb=512", "-timeout=10",
             "-artifact_prefix=$root\fuzz\findings\$Target-")
# Models, textures, saved objects, packages, pictures and effects files are often far larger than
# the default 4 KB.
if ($Target -in "scenegraph", "cobject", "dbpf", "image", "anim", "effects", "hood_drift", "cursor") { $options += "-max_len=65536" }
# Words the format uses, which the fuzzer puts into inputs (fuzz\dict\<target>.dict).
if (Test-Path "$root\fuzz\dict\$Target.dict") { $options += "-dict=$root\fuzz\dict\$Target.dict" }
& "$root\build-fuzz\RelWithDebInfo\${Target}_fuzz.exe" "$root\fuzz\corpus\$Target" @options
