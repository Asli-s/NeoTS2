# Writes the build's version into a header, run on every build: "<release>+<commit>" for a release
# build, "<release>-dev.<count>+<commit>" for any other, with "-modified" when files differ from the
# commit. The count is the number of commits since the last release tag (v1.2.3), or since the first
# commit before there is one, so dev builds sort by it.
# A build stops rather than stamp a version that could mislead (see docs/code-guide.md, Versions).
# The header only changes when the version does, so nothing rebuilds needlessly.
# Called with -DSOURCE=<repository> -DOUTPUT=<header> -DRELEASE=<x.y.z> -DIS_RELEASE=<ON|OFF>.
if(NOT RELEASE MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "The release number must be major.minor.patch, not '${RELEASE}'")
endif()

function(git out)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${SOURCE} ${ARGN}
                    OUTPUT_VARIABLE text OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE failed ERROR_QUIET)
    if(failed)
        set(text "")
    endif()
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

set(commit "")
set(modified FALSE)
set(shallow FALSE)
set(count 0)
find_package(Git QUIET)
if(GIT_FOUND)
    git(commit rev-parse --short=10 HEAD)
endif()
if(commit)
    git(changes status --porcelain --untracked-files=no)
    if(changes)
        set(modified TRUE)
    endif()
    # A shallow copy lacks the older commits and tags, so its count would be wrong.
    git(shallow_text rev-parse --is-shallow-repository)
    if(shallow_text STREQUAL "true")
        set(shallow TRUE)
    endif()
    git(tag describe --tags --match "v[0-9]*" --abbrev=0 HEAD)
    if(tag)
        git(counted rev-list --count ${tag}..HEAD)
    else()
        git(counted rev-list --count HEAD)
    endif()
    if(counted MATCHES "^[0-9]+$")
        set(count ${counted})
    endif()
    # This number's release tag, when it is made, and whether this commit is it or comes after it.
    git(released rev-parse -q --verify "refs/tags/v${RELEASE}^{commit}")
    git(head rev-parse HEAD)
    set(after_release FALSE)
    if(released AND NOT released STREQUAL head)
        execute_process(COMMAND ${GIT_EXECUTABLE} -C ${SOURCE} merge-base --is-ancestor ${released} HEAD
                        RESULT_VARIABLE not_ancestor OUTPUT_QUIET ERROR_QUIET)
        if(NOT not_ancestor)
            set(after_release TRUE)
        endif()
    endif()
endif()

if(IS_RELEASE)
    # A release's version must name exactly the code it was built from, and only once.
    if(NOT commit)
        message(FATAL_ERROR "A release build needs the git repository, to name its commit")
    endif()
    if(modified)
        message(FATAL_ERROR "A release build can't have uncommitted changes: commit them first")
    endif()
    if(shallow)
        message(FATAL_ERROR "A release build needs the whole history (this copy is shallow): git fetch --unshallow")
    endif()
    if(released AND NOT released STREQUAL head)
        message(FATAL_ERROR "v${RELEASE} is already released from another commit: raise the number in CMakeLists.txt")
    endif()
    set(version "${RELEASE}+${commit}")
    set(release_build 1)
    set(file_build 0)
else()
    # Builds after a release must carry the next number, or they would sort before that release.
    if(after_release)
        message(FATAL_ERROR "v${RELEASE} is already released: raise the number in CMakeLists.txt to the next release")
    endif()
    if(NOT commit)
        set(commit "unknown")
    elseif(modified)
        set(commit "${commit}-modified")
    endif()
    if(shallow)
        set(version "${RELEASE}-dev.shallow+${commit}")
        set(file_build 0)
    else()
        set(version "${RELEASE}-dev.${count}+${commit}")
        # Windows keeps each part of a file version in 16 bits.
        set(file_build ${count})
        if(file_build GREATER 65535)
            set(file_build 65535)
        endif()
    endif()
    set(release_build 0)
endif()

string(REPLACE "." ";" parts ${RELEASE})
list(GET parts 0 major)
list(GET parts 1 minor)
list(GET parts 2 patch)
foreach(part ${major} ${minor} ${patch})
    if(part GREATER 65535)
        message(FATAL_ERROR "Each part of the release number must be at most 65535 (Windows keeps them in 16 bits)")
    endif()
endforeach()
file(WRITE ${OUTPUT}.new "// Made by cmake/version.cmake on every build. Also read by app/icon/neots2.rc.
#pragma once
#define NEOTS2_VERSION \"${version}\"
#define NEOTS2_VERSION_MAJOR ${major}
#define NEOTS2_VERSION_MINOR ${minor}
#define NEOTS2_VERSION_PATCH ${patch}
#define NEOTS2_VERSION_BUILD ${file_build}
#define NEOTS2_RELEASE_BUILD ${release_build}
")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OUTPUT}.new ${OUTPUT})
file(REMOVE ${OUTPUT}.new)
