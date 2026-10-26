# Writes a C++ source file holding the bytes of a file, for fourfold_embed()
# (cmake/FourfoldComponent.cmake). Run as a script:
#   cmake -DIN=<file> -DOUT=<file.cpp> -DSYMBOL=<name> -P Embed.cmake
# It defines, in namespace fourfold:
#   extern const unsigned char <name>[];   extern const size_t <name>Size;

file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hexLength)
math(EXPR size "${hexLength} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
# Break the array into lines of 32 bytes, to keep compilers and editors happy.
string(REGEX REPLACE "((0x..,){32})" "\\1\n    " bytes "${bytes}")
file(WRITE "${OUT}.tmp"
"// Generated from ${IN} by cmake/Embed.cmake. Don't edit.
#include <cstddef>
namespace fourfold {
extern const unsigned char ${SYMBOL}[];
extern const size_t ${SYMBOL}Size;
const unsigned char ${SYMBOL}[] = {
    ${bytes}0x00};
const size_t ${SYMBOL}Size = ${size};
}  // namespace fourfold
")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
