# Writes the build's version (its git commit, "-modified" when files differ from it) into a header,
# run on every build; the header only changes when the version does, so nothing rebuilds needlessly.
# Called with -DSOURCE=<repository> -DOUTPUT=<header>.
set(version "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${SOURCE} rev-parse --short=10 HEAD
                    OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE failed ERROR_QUIET)
    if(NOT failed AND commit)
        set(version ${commit})
        execute_process(COMMAND ${GIT_EXECUTABLE} -C ${SOURCE} status --porcelain --untracked-files=no
                        OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(changes)
            set(version "${version}-modified")
        endif()
    endif()
endif()
file(WRITE ${OUTPUT}.new "#pragma once\n// Made by cmake/version.cmake on every build.\n#define NEOTS2_VERSION \"${version}\"\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OUTPUT}.new ${OUTPUT})
file(REMOVE ${OUTPUT}.new)
