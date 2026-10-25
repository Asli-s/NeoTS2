# Makes the GLSL 1.20 of one HLSL shader for the OpenGL 2 renderer: dxc makes SPIR-V, SPIRV-Cross
# turns that into GLSL with its uniform blocks flattened to arrays (GLSL 1.20 has no blocks), and
# the varyings are named alike in both stages so the program links.
#   cmake -DDXC= -DSPIRV_CROSS= -DSOURCE= -DPROFILE= -DSPIRV= -DOUTPUT= -DSTAGE=vert|frag -P glsl_shader.cmake
execute_process(
    COMMAND ${DXC} -nologo -spirv -fspv-target-env=vulkan1.1 -DGL2=1 -T ${PROFILE} -E main -Fo ${SPIRV} ${SOURCE}
    RESULT_VARIABLE result ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "dxc failed on ${SOURCE}:\n${errors}")
endif()
set(options --version 120 --flatten-ubo --remove-unused-variables)
if(STAGE STREQUAL "vert")
    # GL's clip space is -w .. w in depth, where Vulkan's and Direct3D's is 0 .. w.
    list(APPEND options --fixup-clipspace)
endif()
execute_process(
    COMMAND ${SPIRV_CROSS} ${SPIRV} ${options}
    OUTPUT_VARIABLE glsl RESULT_VARIABLE result ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "spirv-cross failed on ${SOURCE}:\n${errors}")
endif()
# A vertex shader's outputs and a fragment shader's inputs are the same varyings, named by their semantic.
if(STAGE STREQUAL "vert")
    string(REPLACE "out_var_" "v_" glsl "${glsl}")
else()
    string(REPLACE "in_var_" "v_" glsl "${glsl}")
endif()
file(WRITE ${OUTPUT} "${glsl}")
