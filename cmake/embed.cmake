# Turns a file into a C++ byte array: cmake -DIN=... -DOUT=... -DNAME=... -P embed.cmake
file(READ "${IN}" HEX HEX)
string(LENGTH "${HEX}" LEN)
math(EXPR SIZE "${LEN} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEX}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" BYTES "${BYTES}")
file(WRITE "${OUT}" "// Generated from ${IN} - do not edit.\n#include <cstddef>\nextern const unsigned char ${NAME}[] = {\n${BYTES}0x00};\nextern const size_t ${NAME}Size = ${SIZE};\n")
