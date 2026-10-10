cmake_minimum_required(VERSION 3.20)
project(NeoTS2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NEOTS2_FUZZ "Build libFuzzer targets with AddressSanitizer instead of the tools" OFF)

if(NEOTS2_FUZZ)
    if(MSVC)
        add_compile_options(/fsanitize=address /fsanitize=fuzzer /Zi)
        add_link_options(/INCREMENTAL:NO /DEBUG)
    else()
        add_compile_options(-fsanitize=address,fuzzer-no-link -g)
        add_link_options(-fsanitize=address,fuzzer)
    endif()
endif()

add_library(project_warnings INTERFACE)
if(MSVC)
    target_compile_options(project_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(project_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# The original Lua 5.0.3 interpreter, for the game's object scripts (third-party, MIT).
file(GLOB LUA_SOURCES third_party/lua-5.0.3/src/*.c third_party/lua-5.0.3/src/lib/*.c)
add_library(lua50 STATIC ${LUA_SOURCES})
target_include_directories(lua50 PUBLIC third_party/lua-5.0.3/include PRIVATE third_party/lua-5.0.3/src)
if(MSVC)
    target_compile_options(lua50 PRIVATE /W0)
    target_compile_definitions(lua50 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(ts2formats
    src/formats/array3d.cpp
    src/formats/terrain.cpp
    src/formats/fami.cpp
    src/formats/famt.cpp
    src/formats/swaf.cpp
    src/formats/propset.cpp
    src/formats/ttab.cpp
    src/formats/luascript.cpp
    src/formats/sdsc.cpp
    src/formats/slot.cpp
    src/formats/srel.cpp
    src/formats/ngbh.cpp
    src/formats/datatables.cpp
    src/formats/scenegraph.cpp
    src/formats/bcon.cpp
    src/formats/bhav.cpp
    src/formats/dbpf.cpp
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
    src/formats/nhood_paint_rules.cpp
    src/formats/lot_description.cpp
    src/formats/nhood_objects.cpp
    src/formats/ui_script.cpp
)
target_include_directories(ts2formats PUBLIC src)
# stb_image (third-party, public domain / MIT) decodes the UI's PNG, JPEG and bitmap images.
add_library(stb_image STATIC src/formats/image.cpp)
target_include_directories(stb_image PRIVATE src third_party/stb)
target_link_libraries(ts2formats PUBLIC stb_image)
target_link_libraries(ts2formats PRIVATE project_warnings)

add_library(ts2content
    src/content/install.cpp
    src/content/cameras.cpp
    src/content/lighting.cpp
    src/content/material_scripts.cpp
    src/content/nhood_catalog.cpp
    src/content/build_scripts.cpp
    src/content/lot.cpp
    src/content/neighborhood.cpp
    src/content/wants.cpp
    src/content/scripts.cpp
    src/content/resource_index.cpp
    src/content/ui_data.cpp
)
target_link_libraries(ts2content PUBLIC ts2formats PRIVATE project_warnings)

add_library(ts2sim
    src/sim/index_code_source.cpp
    src/sim/lot_world.cpp
    src/sim/lua_runtime.cpp
    src/sim/rooms.cpp
    src/sim/vm.cpp
    src/sim/world.cpp
)
target_link_libraries(ts2sim PUBLIC ts2content PRIVATE lua50 project_warnings)

# Scenes to draw, built from the game's models and lots, independent of any graphics API.
add_library(ts2scene
    src/scene/scene.cpp
    src/scene/assets.cpp
    src/scene/builder.cpp
    src/scene/lot_scene.cpp
    src/scene/walls.cpp
    src/scene/roofs.cpp
    src/scene/roof_grid.cpp
    src/scene/round_roofs.cpp
    src/scene/imposters.cpp
    src/scene/neighborhood_scene.cpp
    src/scene/neighborhood_paint.cpp
    src/scene/neighborhood_lighting.cpp
    src/scene/view.cpp
)
target_link_libraries(ts2scene PUBLIC ts2content PRIVATE project_warnings)

if(NEOTS2_FUZZ)
    foreach(target qfs_fuzz dbpf_fuzz bhav_fuzz str_fuzz bcon_fuzz glob_fuzz objd_fuzz
                   objt_fuzz objm_fuzz xobj_fuzz wallgraph_fuzz array3d_fuzz srel_fuzz sdsc_fuzz fami_fuzz famt_fuzz slot_fuzz swaf_fuzz propset_fuzz ttab_fuzz luascript_fuzz ngbh_fuzz datatables_fuzz scenegraph_fuzz stringmap_fuzz walllayer_fuzz namemap_fuzz fencepostlayer_fuzz roofs_fuzz terrain_fuzz)
        add_executable(${target} fuzz/${target}.cpp)
        target_link_libraries(${target} PRIVATE ts2formats project_warnings)
    endforeach()
else()
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

    add_executable(simrun tools/simrun/main.cpp)
    target_link_libraries(simrun PRIVATE ts2sim project_warnings)

    enable_testing()
    add_executable(vm_tests tests/vm_tests.cpp)
    target_link_libraries(vm_tests PRIVATE ts2sim ts2scene lua50 project_warnings)
    add_test(NAME vm_tests COMMAND vm_tests)

    add_executable(fuzzseeds tools/fuzzseeds/main.cpp)
    target_link_libraries(fuzzseeds PRIVATE ts2formats project_warnings)

    # The renderer and the game app need SDL3 (set SDL3_DIR to its cmake folder) and a shader
    # compiler for HLSL (dxc, from the Windows SDK on Windows).
    find_package(SDL3 CONFIG QUIET)
    find_program(NEOTS2_DXC dxc)
    if(NOT NEOTS2_DXC AND WIN32)
        file(GLOB sdk_dxc "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxc.exe")
        list(SORT sdk_dxc ORDER DESCENDING)
        list(GET sdk_dxc 0 NEOTS2_DXC)
    endif()
    if(SDL3_FOUND AND NEOTS2_DXC)
        set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
        set(shader_outputs)
        foreach(shader scene.vert scene.frag ui.vert ui.frag terrain.vert terrain.frag sky.vert sky.frag water.vert water.frag road.vert road.frag imposter.vert imposter.frag)
            if(shader MATCHES "vert$")
                set(profile vs_6_0)
            else()
                set(profile ps_6_0)
            endif()
            set(source ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${shader}.hlsl)
            set(output ${shader_dir}/${shader}.dxil)
            add_custom_command(OUTPUT ${output}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
                COMMAND ${NEOTS2_DXC} -nologo -T ${profile} -E main -Fo ${output} ${source}
                DEPENDS ${source}
                COMMENT "Compiling ${shader}.hlsl")
            list(APPEND shader_outputs ${output})
        endforeach()
        add_custom_target(neots2_shaders DEPENDS ${shader_outputs})

        add_library(ts2render src/render/renderer.cpp)
        target_link_libraries(ts2render PUBLIC ts2scene SDL3::SDL3 PRIVATE project_warnings)

        # The game's UI: its fonts are drawn with FreeType (set freetype_DIR to its cmake folder).
        find_package(freetype CONFIG REQUIRED)
        add_library(ts2ui src/ui/fonts.cpp src/ui/screen.cpp)
        target_link_libraries(ts2ui PUBLIC ts2content PRIVATE freetype project_warnings)

        add_executable(neots2 app/main.cpp app/menu.cpp)
        target_link_libraries(neots2 PRIVATE ts2render ts2ui project_warnings)
        add_dependencies(neots2 neots2_shaders)
        # The app loads its shaders and SDL from beside itself.
        add_custom_command(TARGET neots2 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${shader_dir} $<TARGET_FILE_DIR:neots2>/shaders
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:neots2>)
    else()
        message(STATUS "SDL3 or dxc not found: the renderer and the neots2 app are not built")
    endif()
endif()

# Machine-specific additions, if any.
include(${CMAKE_CURRENT_SOURCE_DIR}/local.cmake OPTIONAL)
