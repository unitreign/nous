// Compile miniz (C code) as C++ so the IDF toolchain's C++-only flags
// (e.g. -fuse-cxa-atexit) do not cause errors when compiling .c files.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
extern "C" {
#include "../../third_party/miniz/miniz.c"
#include "../../third_party/miniz/miniz_tdef.c"
#include "../../third_party/miniz/miniz_tinfl.c"
#include "../../third_party/miniz/miniz_zip.c"
}
#pragma GCC diagnostic pop
