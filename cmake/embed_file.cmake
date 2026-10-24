# Writes a file's bytes into a C++ source as an array, so a program carries the file inside itself.
# Usage: cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DNAME=<array name> -P embed_file.cmake
file(READ ${INPUT} hex HEX)
string(LENGTH "${hex}" length)
math(EXPR size "${length} / 2")
# 8 bytes a line, so the compiler never meets one huge line.
string(REGEX REPLACE "(([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f]))" "\\1 " hex "${hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REPLACE ", 0x" ",\n0x" bytes "${bytes}")
file(WRITE ${OUTPUT}.new
    "#include <cstddef>\n"
    "extern const unsigned char ${NAME}[] = {${bytes}};\n"
    "extern const std::size_t ${NAME}Size = ${size};\n")
configure_file(${OUTPUT}.new ${OUTPUT} COPYONLY)
