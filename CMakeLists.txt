cmake_minimum_required(VERSION 3.20)
project(NeoTS2 VERSION 0.2.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NEOTS2_RELEASE "Stamp this build as the release in project() above, not a -dev build (see docs/code-guide.md)" OFF)
option(NEOTS2_FUZZ "Build libFuzzer targets with AddressSanitizer instead of the tools" OFF)
option(NEOTS2_ASAN "Build everything with AddressSanitizer, to catch memory errors while testing" OFF)
option(NEOTS2_COVERAGE "Measure which lines the tests run (Clang only; see tools/coverage.ps1)" OFF)
option(NEOTS2_STRICT "Treat compiler warnings as errors" OFF)
option(NEOTS2_HARDEN "Build with Windows exploit protections (Control Flow Guard, CET shadow stacks)" ON)
option(NEOTS2_ANALYZE "Run the compiler's code analysis on our code (MSVC; slow, for a separate check build)" OFF)
option(NEOTS2_STL_CHECKS "Check every index into standard containers (vector, array, span, string...) in all builds" ON)

# An index past a container's end stops the program at once instead of reading or writing memory
# it shouldn't, so a bug a broken file reaches can't go further. Each standard library has its own switch.
if(NEOTS2_STL_CHECKS)
    add_compile_definitions(_MSVC_STL_HARDENING=1 _GLIBCXX_ASSERTIONS _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)
endif()

if(NEOTS2_FUZZ)
    if(MSVC)
        add_compile_options(/fsanitize=address /fsanitize=fuzzer /Zi)
        add_link_options(/INCREMENTAL:NO /DEBUG)
    else()
        add_compile_options(-fsanitize=address,fuzzer-no-link -g)
        add_link_options(-fsanitize=address,fuzzer)
    endif()
endif()

if(NEOTS2_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "NEOTS2_COVERAGE needs Clang (clang-cl on Windows)")
    endif()
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    if(MSVC)
        # clang-cl doesn't link its profile runtime itself when the linker is run directly.
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} /clang:-print-resource-dir OUTPUT_VARIABLE clang_resources
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
        link_libraries("${clang_resources}/lib/windows/clang_rt.profile-x86_64.lib")
    else()
        add_link_options(-fprofile-instr-generate)
    endif()
endif()

if(NEOTS2_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address /Zi)
        add_link_options(/INCREMENTAL:NO /DEBUG)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# Exploit protections for the programs people run, so that a bug a crafted file reaches is much harder
# to turn into running code. Test builds with sanitizers or coverage leave them out.
if(NEOTS2_HARDEN AND MSVC AND NOT NEOTS2_FUZZ AND NOT NEOTS2_ASAN AND NOT NEOTS2_COVERAGE)
    add_compile_options(/guard:cf)
    add_link_options(/guard:cf /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT)
endif()

# Symbols for release builds too, so the crash reports testers send can be read (keep each release's
# .pdb files; they needn't ship). /OPT keeps the program as small and fast as without them.
if(MSVC AND NOT NEOTS2_FUZZ AND NOT NEOTS2_ASAN AND NOT NEOTS2_COVERAGE)
    # /FS: several compilers (or builds) may write one .pdb at once.
    add_compile_options($<$<CONFIG:Release>:/Zi> $<$<CONFIG:Release>:/FS>)
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

add_library(project_warnings INTERFACE)
if(MSVC)
    # /sdl: extra security checks and warnings in our own code (third-party code is left as it is).
    target_compile_options(project_warnings INTERFACE /W4 /permissive- /utf-8 $<$<CXX_COMPILER_ID:MSVC>:/sdl>
                           $<$<BOOL:${NEOTS2_STRICT}>:/WX>)
else()
    target_compile_options(project_warnings INTERFACE -Wall -Wextra -Wpedantic $<$<BOOL:${NEOTS2_STRICT}>:-Werror>)
endif()
# The compiler's code analysis on our own code (headers in <> such as SDL's and the standard library's
# are left out). Much slower to build, so for an occasional check build of its own, not every build.
if(NEOTS2_ANALYZE AND MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(project_warnings INTERFACE /analyze /analyze:external- /external:anglebrackets /external:W0)
endif()
# Float maths rounded step by step, never fused into one multiply-add, so results that copy the
# game's exactly (the indoor lighting, for one) come out the same with every compiler. MSVC already
# does this.
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(project_warnings INTERFACE /clang:-ffp-contract=off)
elseif(NOT MSVC)
    target_compile_options(project_warnings INTERFACE -ffp-contract=off)
endif()

# The original Lua 5.0.3 interpreter, for the game's object scripts (third-party, MIT).
file(GLOB LUA_SOURCES third_party/lua-5.0.3/src/*.c third_party/lua-5.0.3/src/lib/*.c)
# Its memory goes through src/sim/lua_memory.c, which caps how much one script state may hold.
# It is built as C++ so that its errors are exceptions (src/sim/lua_error.h): a longjmp would skip
# the destructors of C++ functions that call it.
add_library(lua50 STATIC ${LUA_SOURCES} src/sim/lua_memory.c)
set_source_files_properties(${LUA_SOURCES} src/sim/lua_memory.c PROPERTIES LANGUAGE CXX)
target_include_directories(lua50 PUBLIC third_party/lua-5.0.3/include PRIVATE third_party/lua-5.0.3/src)
if(MSVC)
    target_compile_options(lua50 PRIVATE /W0 /FI${CMAKE_CURRENT_SOURCE_DIR}/src/sim/lua_memory.h
                                         /FI${CMAKE_CURRENT_SOURCE_DIR}/src/sim/lua_error.h)
    target_compile_definitions(lua50 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(lua50 PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/sim/lua_memory.h"
                                         "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/sim/lua_error.h")
endif()

# The run's log and the text of crash reports.
add_library(ts2diag src/diag/log.cpp src/diag/report.cpp src/diag/timing.cpp src/diag/upload.cpp)
target_include_directories(ts2diag PUBLIC src)
target_link_libraries(ts2diag PRIVATE project_warnings)

# Finds the game's installs and save folders without opening any game file. The launcher uses it,
# so it must never link the engine's other libraries.
add_library(ts2paths src/paths/steam_files.cpp src/paths/game_folders.cpp src/paths/save_folders.cpp
            src/paths/new_save_folder.cpp)
target_include_directories(ts2paths PUBLIC src)
target_link_libraries(ts2paths PRIVATE project_warnings)
if(WIN32)
    target_sources(ts2paths PRIVATE src/paths/windows_folders.cpp)
    target_link_libraries(ts2paths PRIVATE shell32 ole32)
endif()

# The launcher's settings file. Like ts2paths, it must never link the engine's other libraries.
add_library(ts2settings src/settings/launcher_settings.cpp)
target_include_directories(ts2settings PUBLIC src)
target_link_libraries(ts2settings PRIVATE project_warnings)

add_library(ts2formats
    src/formats/array3d.cpp
    src/formats/terrain.cpp
    src/formats/fami.cpp
    src/formats/famt.cpp
    src/formats/swaf.cpp
    src/formats/propset.cpp
    src/formats/anim.cpp
    src/formats/reference_list.cpp
    src/formats/ttab.cpp
    src/formats/luascript.cpp
    src/formats/sdsc.cpp
    src/formats/slot.cpp
    src/formats/objf.cpp
    src/formats/simi.cpp
    src/formats/neighbor_remap.cpp
    src/formats/weather.cpp
    src/formats/neighborhood_id.cpp
    src/formats/srel.cpp
    src/formats/ngbh.cpp
    src/formats/datatables.cpp
    src/formats/scenegraph.cpp
    src/formats/bcon.cpp
    src/formats/fcns.cpp
    src/formats/bhav.cpp
    src/formats/dbpf.cpp
    src/formats/dbpf_writer.cpp
    src/formats/glob.cpp
    src/formats/hash.cpp
    src/formats/objd.cpp
    src/formats/objm.cpp
    src/formats/objt.cpp
    src/formats/qfs.cpp
    src/formats/resource_types.cpp
    src/formats/str.cpp
    src/formats/stringmap.cpp
    src/formats/namemap.cpp
    src/formats/fencepostlayer.cpp
    src/formats/roofs.cpp
    src/formats/walllayer.cpp
    src/formats/wallgraph.cpp
    src/formats/xobj.cpp
    src/formats/tga.cpp
    src/formats/reia.cpp
    src/formats/nhood_terrain.cpp
    src/formats/camera_config.cpp
    src/formats/cobject.cpp
    src/formats/nhood_paint_rules.cpp
    src/formats/lot_description.cpp
    src/formats/nhood_objects.cpp
    src/formats/effects.cpp
    src/formats/ui_script.cpp
    src/formats/cursor.cpp
)
target_include_directories(ts2formats PUBLIC src)
# stb_image (third-party, public domain / MIT) decodes the UI's PNG and bitmap images; jpeg.cpp
# decodes JPEGs as the game does, stb taking the kinds it does not read.
add_library(stb_image STATIC src/formats/image.cpp src/formats/jpeg.cpp)
target_include_directories(stb_image PRIVATE src third_party/stb)
target_link_libraries(ts2formats PUBLIC stb_image)
# image.cpp calls tga::decode, which is in ts2formats. The two static libraries need each other, so
# CMake repeats them on the link line; without this GNU ld, which reads archives once in order, fails.
target_link_libraries(stb_image PRIVATE ts2formats)
target_link_libraries(ts2formats PRIVATE project_warnings)

add_library(ts2content
    src/content/animation_lists.cpp
    src/content/install.cpp
    src/content/save_folder.cpp
    src/content/thumbnail_merge.cpp
    src/content/lot_import.cpp
    src/content/cameras.cpp
    src/content/lighting.cpp
    src/content/lighting_state.cpp
    src/content/covering_colors.cpp
    src/content/object_thumbnails.cpp
    src/content/material_scripts.cpp
    src/content/nhood_catalog.cpp
    src/content/build_scripts.cpp
    src/content/lot.cpp
    src/content/neighborhood.cpp
    src/content/wants.cpp
    src/content/scripts.cpp
    src/content/resource_index.cpp
    src/content/ui_data.cpp
    src/content/ui_meters.cpp
    src/content/ui_queue.cpp
    src/content/ui_text.cpp
    src/content/target_pictures.cpp
)
target_link_libraries(ts2content PUBLIC ts2formats ts2diag ts2paths PRIVATE project_warnings)

add_library(ts2sim
    src/sim/index_code_source.cpp
    src/sim/lot_world.cpp
    src/sim/lot_simulation.cpp
    src/sim/ground_click.cpp
    src/sim/route_portals.cpp
    src/sim/pie_menu.cpp
    src/sim/lua_runtime.cpp
    src/sim/lua_script_values.cpp
    src/sim/lua_game_state.cpp
    src/sim/lua_object_manager.cpp
    src/sim/lua_data_tables.cpp
    src/sim/lua_objects.cpp
    src/sim/lua_world.cpp
    src/sim/lua_inventory.cpp
    src/sim/rooms.cpp
    src/sim/vm.cpp
    src/sim/vm_values.cpp
    src/sim/vm_sims.cpp
    src/sim/vm_objects.cpp
    src/sim/vm_inventory.cpp
    src/sim/vm_set_to_next.cpp
    src/sim/vm_placement.cpp
    src/sim/vm_animation.cpp
    src/sim/vm_route_blocked.cpp
    src/sim/vm_routing.cpp
    src/sim/vm_interactions.cpp
    src/sim/animation.cpp
    src/sim/clock.cpp
    src/sim/route_grid.cpp
    src/sim/route_destinations.cpp
    src/sim/route_goals.cpp
    src/sim/route_search.cpp
    src/sim/path_passes.cpp
    src/sim/ray_trace.cpp
    src/sim/route_items.cpp
    src/sim/object_footprint.cpp
    src/sim/type_signature.cpp
    src/sim/object_reset.cpp
    src/sim/route_world.cpp
    src/sim/find_location.cpp
    src/sim/forget_object.cpp
    src/sim/snap.cpp
    src/sim/in_view.cpp
    src/sim/happiness.cpp
    src/sim/motive_update.cpp
    src/sim/advertisement.cpp
    src/sim/advertisers.cpp
    src/sim/find_best_interaction.cpp
    src/sim/find_best_object.cpp
    src/sim/idle_spot.cpp
    src/sim/overlay_events.cpp
    src/sim/outside_grid.cpp
    src/sim/turn.cpp
    src/sim/walk_path.cpp
    src/sim/walk_plan.cpp
    src/sim/walk_sweep.cpp
    src/sim/world.cpp
)
target_link_libraries(ts2sim PUBLIC ts2content PRIVATE ts2scene lua50 project_warnings)

# Scenes to draw, built from the game's models and lots, independent of any graphics API.
add_library(ts2scene
    src/scene/scene.cpp
    src/scene/hood_drift.cpp
    src/scene/assets.cpp
    src/scene/builder.cpp
    src/scene/lot_scene.cpp
    src/scene/walls.cpp
    src/scene/roofs.cpp
    src/scene/roof_grid.cpp
    src/scene/round_roofs.cpp
    src/scene/imposters.cpp
    src/scene/lot_skirt.cpp
    src/scene/pool_water.cpp
    src/scene/object_sun.cpp
    src/scene/pick.cpp
    src/scene/object_lights.cpp
    src/scene/outfit_properties.cpp
    src/scene/sims.cpp
    src/scene/texture_blend.cpp
    src/scene/normal_map.cpp
    src/scene/incidence_map.cpp
    src/scene/sim_builder.cpp
    src/scene/portrait.cpp
    src/scene/limb_ik.cpp
    src/scene/rig.cpp
    src/scene/scene_ray.cpp
    src/scene/rig_layers.cpp
    src/scene/animation_library.cpp
    src/scene/neighborhood_scene.cpp
    src/scene/ground_decal.cpp
    src/scene/neighborhood_decals.cpp
    src/scene/neighborhood_plumbobs.cpp
    src/scene/neighborhood_paint.cpp
    src/scene/neighborhood_lighting.cpp
    src/scene/floor_light.cpp
    src/scene/light_grid.cpp
    src/scene/wall_runs.cpp
    src/scene/lamp_lights.cpp
    src/scene/portal_records.cpp
    src/scene/room_samples.cpp
    src/scene/wall_light.cpp
    src/scene/light_corners.cpp
    src/scene/lot_lighting.cpp
    src/scene/lot_light_objects.cpp
    src/scene/light_map_texels.cpp
    src/scene/lot_light_maps.cpp
    src/scene/lot_shadow.cpp
    src/scene/lot_shadow_inputs.cpp
    src/scene/room_light.cpp
    src/scene/window_light.cpp
    src/scene/portable_math.cpp
    src/scene/view.cpp
)
target_link_libraries(ts2scene PUBLIC ts2content PRIVATE project_warnings)

# What every renderer shares, apart from any graphics API: the CPU side of a scene, culling and
# draw order, and the numbers each draw's shaders are given.
add_library(ts2renderplan src/render/frame_plan.cpp src/render/mesh_pack.cpp src/render/shared_state.cpp src/render/terrain_mesh.cpp)
target_link_libraries(ts2renderplan PUBLIC ts2scene PRIVATE project_warnings)

if(NEOTS2_FUZZ)
    foreach(target qfs_fuzz dbpf_fuzz cobject_fuzz bhav_fuzz str_fuzz bcon_fuzz fcns_fuzz glob_fuzz objd_fuzz
                   objt_fuzz objm_fuzz xobj_fuzz wallgraph_fuzz array3d_fuzz srel_fuzz sdsc_fuzz fami_fuzz famt_fuzz slot_fuzz swaf_fuzz propset_fuzz ttab_fuzz luascript_fuzz ngbh_fuzz datatables_fuzz scenegraph_fuzz stringmap_fuzz walllayer_fuzz namemap_fuzz fencepostlayer_fuzz roofs_fuzz terrain_fuzz reia_fuzz
                   anim_fuzz image_fuzz lot_description_fuzz nhood_objects_fuzz nhood_terrain_fuzz objf_fuzz reference_list_fuzz simi_fuzz weather_fuzz neighborhood_id_fuzz camera_config_fuzz nhood_paint_rules_fuzz ui_script_fuzz cursor_fuzz effects_fuzz
                   neighbor_remap_fuzz)
        add_executable(${target} fuzz/${target}.cpp)
        target_link_libraries(${target} PRIVATE ts2formats project_warnings)
    endforeach()
    # Engine code that is not a file reader but takes values from lots and objects.
    add_executable(ground_decal_fuzz fuzz/ground_decal_fuzz.cpp src/scene/ground_decal.cpp)
    target_link_libraries(ground_decal_fuzz PRIVATE ts2formats project_warnings)
    add_executable(hood_drift_fuzz fuzz/hood_drift_fuzz.cpp src/scene/hood_drift.cpp)
    target_link_libraries(hood_drift_fuzz PRIVATE ts2formats project_warnings)
    add_executable(floorlight_fuzz fuzz/floorlight_fuzz.cpp src/scene/floor_light.cpp)
    target_link_libraries(floorlight_fuzz PRIVATE ts2formats ts2diag project_warnings)
    add_executable(walllight_fuzz fuzz/walllight_fuzz.cpp src/scene/wall_light.cpp src/scene/light_corners.cpp src/scene/floor_light.cpp src/scene/portable_math.cpp)
    target_link_libraries(walllight_fuzz PRIVATE ts2formats ts2diag project_warnings)
    add_executable(windowlight_fuzz fuzz/windowlight_fuzz.cpp src/scene/window_light.cpp)
    target_link_libraries(windowlight_fuzz PRIVATE ts2formats ts2diag project_warnings)
    add_executable(lightgrid_fuzz fuzz/lightgrid_fuzz.cpp src/scene/light_grid.cpp src/scene/wall_runs.cpp src/scene/floor_light.cpp src/content/lot.cpp)
    target_link_libraries(lightgrid_fuzz PRIVATE ts2formats ts2diag project_warnings)
    add_executable(wallruns_fuzz fuzz/wallruns_fuzz.cpp src/scene/wall_runs.cpp src/scene/wall_light.cpp src/scene/portable_math.cpp src/content/lot.cpp)
    target_link_libraries(wallruns_fuzz PRIVATE ts2formats ts2diag project_warnings)
    add_executable(lighting_fuzz fuzz/lighting_fuzz.cpp src/content/lighting.cpp)
    target_link_libraries(lighting_fuzz PRIVATE ts2formats ts2diag project_warnings)
    add_executable(lamplights_fuzz fuzz/lamplights_fuzz.cpp src/scene/lamp_lights.cpp src/scene/portable_math.cpp)
    target_link_libraries(lamplights_fuzz PRIVATE ts2formats ts2diag project_warnings)
    add_executable(portalrecords_fuzz fuzz/portalrecords_fuzz.cpp src/scene/portal_records.cpp src/scene/lamp_lights.cpp src/scene/portable_math.cpp)
    target_link_libraries(portalrecords_fuzz PRIVATE ts2formats ts2diag project_warnings)
    add_executable(roomsamples_fuzz fuzz/roomsamples_fuzz.cpp src/scene/room_samples.cpp src/scene/room_light.cpp src/scene/wall_runs.cpp src/scene/wall_light.cpp src/scene/portable_math.cpp src/content/covering_colors.cpp src/content/lot.cpp)
    target_link_libraries(roomsamples_fuzz PRIVATE ts2formats ts2diag project_warnings)
    add_executable(lotlighting_fuzz fuzz/lotlighting_fuzz.cpp src/scene/lot_shadow.cpp src/scene/neighborhood_lighting.cpp src/scene/lot_lighting.cpp src/scene/light_corners.cpp src/scene/light_grid.cpp src/scene/wall_runs.cpp src/scene/wall_light.cpp src/scene/floor_light.cpp src/scene/room_samples.cpp src/scene/room_light.cpp src/scene/window_light.cpp src/scene/portable_math.cpp src/content/lighting.cpp src/content/lot.cpp)
    target_link_libraries(lotlighting_fuzz PRIVATE ts2formats ts2diag project_warnings)
    add_executable(route_search_fuzz fuzz/route_search_fuzz.cpp src/sim/route_grid.cpp src/sim/route_search.cpp src/sim/path_passes.cpp)
    target_link_libraries(route_search_fuzz PRIVATE ts2formats ts2diag project_warnings)
    add_executable(lua_runtime_fuzz fuzz/lua_runtime_fuzz.cpp)
    target_link_libraries(lua_runtime_fuzz PRIVATE ts2sim lua50 project_warnings)
    add_executable(vm_fuzz fuzz/vm_fuzz.cpp)
    target_link_libraries(vm_fuzz PRIVATE ts2sim project_warnings)
    add_executable(lotlevels_fuzz fuzz/lotlevels_fuzz.cpp src/scene/pool_water.cpp)
    target_link_libraries(lotlevels_fuzz PRIVATE ts2content project_warnings)
    add_executable(objectsun_fuzz fuzz/objectsun_fuzz.cpp src/scene/object_sun.cpp src/scene/object_lights.cpp src/scene/lot_shadow.cpp src/scene/neighborhood_lighting.cpp src/scene/scene.cpp)
    target_link_libraries(objectsun_fuzz PRIVATE ts2content project_warnings)
    add_executable(steam_files_fuzz fuzz/steam_files_fuzz.cpp)
    target_link_libraries(steam_files_fuzz PRIVATE ts2paths project_warnings)
    add_executable(launcher_settings_fuzz fuzz/launcher_settings_fuzz.cpp)
    target_link_libraries(launcher_settings_fuzz PRIVATE ts2settings project_warnings)
else()
    # Reads every resource of an installation with the engine's readers and reports the result.
    add_executable(gamecheck tools/gamecheck/main.cpp)
    target_link_libraries(gamecheck PRIVATE ts2content lua50 project_warnings)

    add_executable(dbpfdump tools/dbpfdump/main.cpp)
    target_link_libraries(dbpfdump PRIVATE ts2formats lua50 project_warnings)


    add_executable(lotview tools/lotview/main.cpp)
    target_link_libraries(lotview PRIVATE ts2sim project_warnings)

    add_executable(lotdump tools/lotdump/main.cpp)
    target_link_libraries(lotdump PRIVATE ts2sim project_warnings)

    add_executable(bhavdump tools/bhavdump/main.cpp)
    target_link_libraries(bhavdump PRIVATE ts2content project_warnings)


    add_executable(uidump tools/uidump/main.cpp)
    target_link_libraries(uidump PRIVATE ts2content project_warnings)

    add_executable(ts2index tools/ts2index/main.cpp)
    target_link_libraries(ts2index PRIVATE ts2content project_warnings)

    add_executable(simrun tools/simrun/main.cpp app/route_sources.cpp)
    target_link_libraries(simrun PRIVATE ts2sim ts2scene project_warnings)

    enable_testing()
    add_executable(vm_tests
        tests/test_main.cpp tests/vm_core_tests.cpp tests/vm_objects_tests.cpp tests/vm_interactions_tests.cpp
        tests/vm_scripts_tests.cpp tests/simulation_tests.cpp tests/route_planning_tests.cpp tests/vm_walking_tests.cpp
        tests/vm_set_to_next_tests.cpp tests/vm_animation_tests.cpp tests/formats_tests.cpp
        tests/models_and_images_tests.cpp tests/scene_tests.cpp tests/assets_tests.cpp tests/lighting_tests.cpp tests/render_plan_tests.cpp tests/content_tests.cpp
        tests/ui_and_diag_tests.cpp tests/paths_tests.cpp tests/fuzz_regressions_tests.cpp src/ui/pie_layout.cpp src/ui/tip_layout.cpp src/ui/font_metrics.cpp src/ui/drag_arrow.cpp app/object_materials.cpp)
    target_link_libraries(vm_tests PRIVATE ts2renderplan ts2sim ts2scene ts2paths ts2settings lua50 project_warnings)
    # Every fuzz target's entry point under its own name, so the tests replay the inputs kept in
    # fuzz/regressions/<target> through the same code the fuzzer ran.
    file(GLOB fuzz_harnesses fuzz/*_fuzz.cpp)
    set(fuzz_entries "")
    foreach(harness ${fuzz_harnesses})
        get_filename_component(fuzz_name ${harness} NAME_WE)
        string(REGEX REPLACE "_fuzz$" "" fuzz_name ${fuzz_name})
        set_source_files_properties(${harness} PROPERTIES COMPILE_DEFINITIONS "LLVMFuzzerTestOneInput=neots2_fuzz_${fuzz_name}")
        string(APPEND fuzz_entries "FUZZ_TARGET(${fuzz_name})\n")
    endforeach()
    file(WRITE ${CMAKE_BINARY_DIR}/generated/fuzz_targets.inc.new "${fuzz_entries}")
    configure_file(${CMAKE_BINARY_DIR}/generated/fuzz_targets.inc.new ${CMAKE_BINARY_DIR}/generated/fuzz_targets.inc COPYONLY)
    target_sources(vm_tests PRIVATE ${fuzz_harnesses})
    target_include_directories(vm_tests PRIVATE ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(vm_tests PRIVATE NEOTS2_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    # The larger test files have more sections than an object file allows by default.
    if(MSVC)
        target_compile_options(vm_tests PRIVATE /bigobj)
    endif()
    # The tests in 4 groups, which ctest -j runs side by side. Each test still runs once.
    set(test_groups 4)
    foreach(group RANGE 1 ${test_groups})
        add_test(NAME vm_tests_${group} COMMAND vm_tests --group ${group}/${test_groups})
    endforeach()

    add_executable(fuzzseeds tools/fuzzseeds/main.cpp)
    target_link_libraries(fuzzseeds PRIVATE ts2formats project_warnings)

    # The renderer and the game app need a window, the keyboard and mouse and an OpenGL context from
    # SDL 3 (set SDL3_DIR to its cmake folder), or from SDL 1.2 where there is no SDL 3 (Mac OS X 10.4 and
    # 10.5 on PowerPC). NEOTS2_PLATFORM picks: sdl3, sdl12, or auto, which takes SDL 3 when it is there (and SDL 1.2 on a Mac).
    # SDL 1.2 has the OpenGL 2 renderer only (the SDL GPU renderer needs SDL 3).
    # They also need a shader compiler for HLSL (dxc, from the Windows SDK on Windows). Windows draws with
    # Direct3D 12 and DXIL shaders, other systems with Vulkan and SPIR-V shaders, which need a dxc built with SPIR-V.
    set(NEOTS2_PLATFORM auto CACHE STRING "Where the window and input come from: sdl3, sdl12 or auto")
    set_property(CACHE NEOTS2_PLATFORM PROPERTY STRINGS auto sdl3 sdl12)
    if(NOT NEOTS2_PLATFORM MATCHES "^(auto|sdl3|sdl12)$")
        message(FATAL_ERROR "NEOTS2_PLATFORM is '${NEOTS2_PLATFORM}': use auto, sdl3 or sdl12")
    endif()
    find_package(SDL3 CONFIG QUIET)
    set(platform_name "")
    if(NEOTS2_PLATFORM STREQUAL "sdl3" OR NEOTS2_PLATFORM STREQUAL "auto")
        if(SDL3_FOUND)
            set(platform_name sdl3)
        endif()
    endif()
    # Auto takes SDL 1.2 on a Mac only: elsewhere SDL 1.2 is there for other reasons, and it is asked for by name.
    if(NOT platform_name AND (NEOTS2_PLATFORM STREQUAL "sdl12" OR (NEOTS2_PLATFORM STREQUAL "auto" AND APPLE)))
        # SDL 1.2 has no CMake package: its header, library and the main() it starts a Mac program from.
        find_path(NEOTS2_SDL12_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL)
        find_library(NEOTS2_SDL12_LIBRARY NAMES SDL)
        find_library(NEOTS2_SDL12_MAIN_LIBRARY NAMES SDLmain)
        if(NEOTS2_SDL12_INCLUDE_DIR AND NEOTS2_SDL12_LIBRARY)
            set(platform_name sdl12)
        endif()
    endif()
    find_program(NEOTS2_DXC dxc)
    set(NEOTS2_SHADER_COMPILER ${NEOTS2_DXC})
    set(NEOTS2_SHADER_COMPILER_KIND dxc)
    if(NOT NEOTS2_DXC AND WIN32)
        file(GLOB sdk_dxc "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxc.exe")
        list(SORT sdk_dxc ORDER DESCENDING)
        list(GET sdk_dxc 0 NEOTS2_DXC)
        set(NEOTS2_SHADER_COMPILER ${NEOTS2_DXC})
    endif()
    # DXC's release archives do not include a macOS executable. Shaderc's glslc accepts the
    # same HLSL sources and produces the SPIR-V that SDL's Metal backend loads on macOS.
    if(NOT NEOTS2_SHADER_COMPILER AND APPLE)
        find_program(NEOTS2_GLSLC glslc)
        if(NEOTS2_GLSLC)
            set(NEOTS2_SHADER_COMPILER ${NEOTS2_GLSLC})
            set(NEOTS2_SHADER_COMPILER_KIND glslc)
        endif()
    endif()
    if(APPLE)
        # SDL's Metal backend takes MSL rather than SPIR-V. SPIRV-Cross converts the
        # intermediate generated by glslc (or DXC) into that format.
        find_program(NEOTS2_SPIRV_CROSS spirv-cross)
    endif()
    if(platform_name AND NEOTS2_SHADER_COMPILER AND (NOT APPLE OR NEOTS2_SPIRV_CROSS))
        add_library(ts2platform src/platform/time.cpp)
        target_include_directories(ts2platform PUBLIC src)
        if(platform_name STREQUAL "sdl3")
            target_sources(ts2platform PRIVATE src/platform/sdl3/platform_sdl3.cpp)
            target_link_libraries(ts2platform PUBLIC SDL3::SDL3 PRIVATE project_warnings)
            target_compile_definitions(ts2platform PUBLIC NEOTS2_PLATFORM_SDL3=1)
        else()
            target_sources(ts2platform PRIVATE src/platform/sdl12/platform_sdl12.cpp src/platform/sdl12/png.cpp src/platform/sdl12/system_unix.cpp)
            target_include_directories(ts2platform PUBLIC ${NEOTS2_SDL12_INCLUDE_DIR})
            # SDLmain goes before SDL itself: it holds the main() that starts the program (a Mac program must be started through it).
            set(sdl12_libraries ${NEOTS2_SDL12_LIBRARY})
            if(NEOTS2_SDL12_MAIN_LIBRARY)
                set(sdl12_libraries ${NEOTS2_SDL12_MAIN_LIBRARY} ${NEOTS2_SDL12_LIBRARY})
            endif()
            target_link_libraries(ts2platform PUBLIC ${sdl12_libraries} PRIVATE project_warnings)
            if(APPLE)
                # What a static SDL 1.2 needs of the system (sdl-config --static-libs).
                target_link_libraries(ts2platform PUBLIC "-framework Cocoa" "-framework OpenGL" "-framework ApplicationServices"
                                      "-framework Carbon" "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudio"
                                      "-framework CoreFoundation" "-framework IOKit")
            endif()
            target_compile_definitions(ts2platform PUBLIC NEOTS2_PLATFORM_SDL12=1)
        endif()
        message(STATUS "Window and input from ${platform_name}")
        set(sdlgpu_enabled OFF)
        if(platform_name STREQUAL "sdl3")
            set(sdlgpu_enabled ON)
        endif()

        set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
        set(shader_outputs)
        # The shaders the OpenGL 2 renderer draws with (skinned ones are posed on the CPU there).
        set(gl2_shaders scene.vert scene.frag ui.vert ui.frag terrain.vert terrain.frag sky.vert sky.frag road.vert road.frag imposter.vert imposter.frag clip.vert clip.frag water.vert water.frag)
        if(sdlgpu_enabled)
        foreach(shader scene.vert skinned.vert scene.frag ui.vert ui.frag terrain.vert terrain.frag sky.vert sky.frag water.vert water.frag road.vert road.frag imposter.vert imposter.frag clip.vert clip.frag)
            if(shader MATCHES "vert$")
                set(profile vs_6_0)
            else()
                set(profile ps_6_0)
            endif()
            set(source ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${shader}.hlsl)
            if(WIN32)
                set(output ${shader_dir}/${shader}.dxil)
                set(shader_target)
            else()
                set(output ${shader_dir}/${shader}.spv)
                set(shader_target -spirv -fspv-target-env=vulkan1.1)
            endif()
            if(NEOTS2_SHADER_COMPILER_KIND STREQUAL "glslc")
                if(shader MATCHES "vert$")
                    set(stage vert)
                else()
                    set(stage frag)
                endif()
                add_custom_command(OUTPUT ${output}
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
                    COMMAND ${NEOTS2_SHADER_COMPILER} -x hlsl -DNEOTS2_SPIRV=1 -fshader-stage=${stage} -fentry-point=main --target-env=vulkan1.1 -o ${output} ${source}
                    DEPENDS ${source} ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/object_light.hlsli
                            ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/sdl_gpu.hlsli
                    COMMENT "Compiling ${shader}.hlsl")
            else()
                add_custom_command(OUTPUT ${output}
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
                    COMMAND ${NEOTS2_SHADER_COMPILER} -nologo ${shader_target} -T ${profile} -E main -Fo ${output} ${source}
                    DEPENDS ${source} ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/object_light.hlsli
                            ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/sdl_gpu.hlsli
                    COMMENT "Compiling ${shader}.hlsl")
            endif()
            list(APPEND shader_outputs ${output})
            if(APPLE)
                set(msl_output ${shader_dir}/${shader}.msl)
                set(msl_binding_options)
                if(shader MATCHES "frag$")
                    # Fragment shaders sample SDL slot n from Metal texture and sampler n.
                    # Keep the HLSL register numbers instead of compacting active resources.
                    set(msl_binding_options --msl-decoration-binding)
                endif()
                add_custom_command(OUTPUT ${msl_output}
                    COMMAND ${NEOTS2_SPIRV_CROSS} ${output} --msl --msl-version 23000 ${msl_binding_options} --output ${msl_output}
                    DEPENDS ${output}
                    COMMENT "Making Metal shader ${shader}.hlsl")
                list(APPEND shader_outputs ${msl_output})
            endif()
        endforeach()
        add_custom_target(neots2_shaders DEPENDS ${shader_outputs})
        endif()

        # The OpenGL 2 renderer (for systems with no more) draws with GLSL 1.20 made from the same HLSL,
        # which needs SPIRV-Cross (the spirv-cross tool, from the Vulkan SDK or a package) and a dxc built with SPIR-V.
        option(NEOTS2_GL2 "Build the OpenGL 2 renderer (needs spirv-cross)" OFF)
        find_program(NEOTS2_SPIRV_CROSS spirv-cross)
        set(gl2_enabled OFF)
        # Without SDL 3 it is the only renderer there is, so it is built whatever NEOTS2_GL2 says.
        set(gl2_wanted ${NEOTS2_GL2})
        if(NOT sdlgpu_enabled)
            set(gl2_wanted ON)
        endif()
        if(gl2_wanted AND NEOTS2_SPIRV_CROSS)
            set(gl2_enabled ON)
            set(gl_shader_outputs)
            foreach(shader ${gl2_shaders})
                if(shader MATCHES "vert$")
                    set(profile vs_6_0)
                    set(stage vert)
                else()
                    set(profile ps_6_0)
                    set(stage frag)
                endif()
                set(source ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${shader}.hlsl)
                set(output ${shader_dir}/${shader}.glsl)
                add_custom_command(OUTPUT ${output}
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
                    COMMAND ${CMAKE_COMMAND} -DDXC=${NEOTS2_DXC} -DSPIRV_CROSS=${NEOTS2_SPIRV_CROSS} -DSOURCE=${source} -DPROFILE=${profile}
                            -DSTAGE=${stage} -DSPIRV=${CMAKE_CURRENT_BINARY_DIR}/gl/${shader}.spv -DOUTPUT=${output}
                            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/glsl_shader.cmake
                    DEPENDS ${source} ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/object_light.hlsli
                            ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/sdl_gpu.hlsli
                            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/glsl_shader.cmake
                    COMMENT "Making GLSL for ${shader}.hlsl")
                list(APPEND gl_shader_outputs ${output})
            endforeach()
            file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gl)
            add_custom_target(neots2_gl_shaders DEPENDS ${gl_shader_outputs})
        elseif(gl2_wanted)
            message(STATUS "spirv-cross not found: the OpenGL 2 renderer is not built")
        endif()
        if(NOT sdlgpu_enabled AND NOT gl2_enabled)
            message(FATAL_ERROR "With ${platform_name} the game draws with the OpenGL 2 renderer, which needs spirv-cross (set NEOTS2_SPIRV_CROSS)")
        endif()

        add_library(ts2render src/render/make_renderer.cpp)
        target_link_libraries(ts2render PUBLIC ts2renderplan ts2platform PRIVATE project_warnings)
        if(sdlgpu_enabled)
            target_sources(ts2render PRIVATE src/render/renderer.cpp src/render/renderer_pipelines.cpp src/render/renderer_scene.cpp
                           src/render/renderer_upload.cpp src/render/renderer_draw.cpp)
            target_link_libraries(ts2render PUBLIC SDL3::SDL3)
            target_compile_definitions(ts2render PUBLIC NEOTS2_SDLGPU=1)
        endif()
        if(gl2_enabled)
            add_library(ts2render_gl src/render/gl/gl_api.cpp src/render/gl/gl_renderer.cpp src/render/gl/gl_shaders.cpp
                        src/render/gl/gl_upload.cpp src/render/gl/gl_draw.cpp src/render/gl/gl_terrain.cpp)
            target_link_libraries(ts2render_gl PUBLIC ts2renderplan ts2platform PRIVATE project_warnings)
            target_link_libraries(ts2render PUBLIC ts2render_gl)
            target_compile_definitions(ts2render PUBLIC NEOTS2_GL2=1)
        endif()

        # The game's UI: its fonts are drawn with FreeType (set freetype_DIR to its cmake folder).
        # Some distributions (Arch) ship no CMake config, so fall back to CMake's own FindFreetype
        # module (which uses pkg-config/headers) and expose it under the same target name.
        find_package(freetype CONFIG QUIET)
        if(NOT TARGET freetype)
            find_package(Freetype REQUIRED)
            add_library(freetype INTERFACE IMPORTED)
            target_link_libraries(freetype INTERFACE Freetype::Freetype)
        endif()
        add_library(ts2ui src/ui/fonts.cpp src/ui/font_metrics.cpp src/ui/screen.cpp src/ui/pie_layout.cpp src/ui/tip_layout.cpp src/ui/drag_arrow.cpp)
        target_link_libraries(ts2ui PUBLIC ts2content PRIVATE freetype project_warnings)
        # Text layout is tested with our own small font (tests/data), so only where the UI builds.
        target_sources(vm_tests PRIVATE tests/text_layout_tests.cpp)
        target_link_libraries(vm_tests PRIVATE ts2ui)
        target_compile_definitions(vm_tests PRIVATE NEOTS2_TEXT_LAYOUT_TESTS)

        add_executable(neots2 app/main.cpp app/menu.cpp app/neighborhood.cpp app/neighborhood_ui.cpp app/lot_life.cpp app/object_materials.cpp app/lot_light.cpp app/lot_daylight.cpp app/route_sources.cpp app/lot_ui.cpp app/live_panel.cpp app/job_panel.cpp app/memories_page.cpp app/age_meter.cpp app/queue_panel.cpp app/pie_menu_ui.cpp app/mood_meter.cpp app/aspiration_bars.cpp app/pause_frame.cpp app/needs_panel.cpp app/personality_page.cpp app/relationships_panel.cpp app/rewards_panel.cpp app/simology_panel.cpp app/interests_page.cpp app/hobbies_page.cpp app/chemistry_page.cpp app/meter_bar.cpp app/skewer.cpp app/wants_panel.cpp app/portraits.cpp app/cursors.cpp
                       app/diagnostics.cpp app/crash_handler.cpp app/crash_dialog.cpp app/unexpected_exit.cpp app/report_upload.cpp app/message_box.cpp app/open_renderer.cpp)
        target_link_libraries(neots2 PRIVATE ts2render ts2ui ts2sim ts2diag project_warnings)
        add_dependencies(neots2 neots2_version)
        if(sdlgpu_enabled)
            add_dependencies(neots2 neots2_shaders)
        endif()
        if(gl2_enabled)
            add_dependencies(neots2 neots2_gl_shaders)
        endif()
        # The build's version (release number and commit), for logs, crash reports, the exe's details and
        # the screen (remade on every build).
        set(version_header ${CMAKE_BINARY_DIR}/generated/neots2_version.hpp)
        add_custom_target(neots2_version
            COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_SOURCE_DIR} -DOUTPUT=${version_header} -DRELEASE=${PROJECT_VERSION}
                    -DIS_RELEASE=${NEOTS2_RELEASE} -P ${CMAKE_SOURCE_DIR}/cmake/version.cmake
            BYPRODUCTS ${version_header}
            COMMENT "Stamping the build's version")
        target_include_directories(neots2 PRIVATE ${CMAKE_BINARY_DIR}/generated)
        if(WIN32)
            # Minidumps and stacks (dbghelp), opening the report's folder and link (shell32), the message
            # (comctl32), what Windows noted of a run that ended without a word (wevtapi).
            # Sending a report (winhttp, and bcrypt for its MD5).
            target_link_libraries(neots2 PRIVATE dbghelp shell32 comctl32 wevtapi winhttp bcrypt)
        endif()
        if(WIN32)
            target_sources(neots2 PRIVATE app/icon/neots2.rc)
        endif()
        # The app loads its shaders and SDL from beside itself (SDL 1.2 is linked in).
        add_custom_command(TARGET neots2 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${shader_dir} $<TARGET_FILE_DIR:neots2>/shaders)
        if(platform_name STREQUAL "sdl3")
            add_custom_command(TARGET neots2 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:neots2>)
        endif()

        # Every header compiles on its own (it includes what it uses), so any of them can be included
        # first. Built on request: cmake --build build --config Release --target header_check
        file(GLOB_RECURSE all_headers CONFIGURE_DEPENDS src/*.hpp app/*.hpp)
        # What is for SDL 3 only (the SDL GPU renderer, the launcher) can't be included where it isn't.
        if(NOT platform_name STREQUAL "sdl3")
            list(FILTER all_headers EXCLUDE REGEX "/(src/platform/sdl3/|src/render/renderer(_internals)?\\.hpp$|app/launcher/)")
        endif()
        set(header_sources "")
        foreach(header ${all_headers})
            file(RELATIVE_PATH name ${CMAKE_SOURCE_DIR} ${header})
            string(REGEX REPLACE "[/.]" "_" name ${name})
            set(source ${CMAKE_BINARY_DIR}/header_check/${name}.cpp)
            file(CONFIGURE OUTPUT ${source} CONTENT "#include \"${header}\"\n")
            list(APPEND header_sources ${source})
        endforeach()
        add_library(header_check OBJECT EXCLUDE_FROM_ALL ${header_sources})
        target_link_libraries(header_check PRIVATE ts2render ts2ui ts2sim lua50 freetype project_warnings)
        target_include_directories(header_check PRIVATE ${CMAKE_BINARY_DIR}/generated)
        add_dependencies(header_check neots2_version)
    else()
        message(STATUS "SDL3, a shader compiler (dxc; glslc on macOS), or spirv-cross on macOS not found: the renderer and the neots2 app are not built")
    endif()

    if(SDL3_FOUND)
        add_library(imgui STATIC third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
                    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
                    third_party/imgui/backends/imgui_impl_sdl3.cpp third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp)
        target_include_directories(imgui PUBLIC third_party/imgui)
        target_link_libraries(imgui PUBLIC SDL3::SDL3)

        # The launcher is its own program. It links only SDL, ImGui, ts2paths and ts2settings, so it opens even
        # when the game's files or the graphics driver are broken.
        # Its fonts are built into the exe, so it needs no font file when it runs.
        set(launcher_font_sources "")
        foreach(font Regular SemiBold)
            set(font_source ${CMAKE_BINARY_DIR}/generated/noto_sans_${font}.cpp)
            add_custom_command(OUTPUT ${font_source}
                COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_SOURCE_DIR}/third_party/fonts/NotoSans-${font}.ttf
                        -DOUTPUT=${font_source} -DNAME=notoSans${font} -P ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
                DEPENDS ${CMAKE_SOURCE_DIR}/third_party/fonts/NotoSans-${font}.ttf ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
                COMMENT "Building NotoSans-${font}.ttf into the launcher")
            list(APPEND launcher_font_sources ${font_source})
        endforeach()

        add_executable(neots2-launcher WIN32 app/launcher/main.cpp app/launcher/launcher_ui.cpp app/launcher/game_start.cpp
                       app/launcher/launcher_style.cpp app/launcher/running_games.cpp ${launcher_font_sources})
        target_include_directories(neots2-launcher PRIVATE app)
        target_link_libraries(neots2-launcher PRIVATE ts2paths ts2settings imgui SDL3::SDL3 project_warnings)
        if(gl2_enabled)
            # So that it offers the OpenGL 2 choice only when the game has that renderer.
            target_compile_definitions(neots2-launcher PRIVATE NEOTS2_GL2=1)
        endif()
        if(WIN32)
            target_sources(neots2-launcher PRIVATE app/launcher/launcher.rc app/launcher/launcher.manifest)
        endif()
        if(MSVC)
            # SDL3.dll is loaded on first use, so the launcher can say what to do when it's missing.
            target_link_options(neots2-launcher PRIVATE /DELAYLOAD:SDL3.dll)
            target_link_libraries(neots2-launcher PRIVATE delayimp comctl32)
        endif()
        add_custom_command(TARGET neots2-launcher POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:neots2-launcher>)

        foreach(target neots2-launcher ts2paths ts2settings)
            get_target_property(linked ${target} LINK_LIBRARIES)
            foreach(library ts2render ts2render_gl ts2renderplan ts2ui ts2sim ts2scene ts2content ts2formats lua50)
                if(library IN_LIST linked)
                    message(FATAL_ERROR "${target} must not link ${library}: the launcher stays apart from the engine")
                endif()
            endforeach()
        endforeach()
    endif()
endif()

# Machine-specific additions, if any.
include(${CMAKE_CURRENT_SOURCE_DIR}/local.cmake OPTIONAL)
