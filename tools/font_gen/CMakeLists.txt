cmake_minimum_required(VERSION 3.16)
project(font_gen CXX C)

option(FONT_GEN_WASM "Build font_gen as a WebAssembly module (requires Emscripten)" OFF)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# ── FreeType ───────────────────────────────────────────────────────────────
FetchContent_Declare(freetype
    GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
    GIT_TAG        VER-2-13-3
    GIT_SHALLOW    TRUE
)
set(FT_DISABLE_ZLIB     ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(freetype)

# ── HarfBuzz ───────────────────────────────────────────────────────────────
FetchContent_Declare(harfbuzz
    GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
    GIT_TAG        10.4.0
    GIT_SHALLOW    TRUE
)
set(HB_BUILD_UTILS          OFF CACHE BOOL "" FORCE)
set(HB_BUILD_TESTS          OFF CACHE BOOL "" FORCE)
set(HB_BUILD_SUBSET         OFF CACHE BOOL "" FORCE)
set(HB_HAVE_FREETYPE        OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GLIB            OFF CACHE BOOL "" FORCE)
set(HB_HAVE_ICU             OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GRAPHITE2       OFF CACHE BOOL "" FORCE)
set(HB_HAVE_UNISCRIBE       OFF CACHE BOOL "" FORCE)
set(HB_HAVE_DIRECTWRITE     OFF CACHE BOOL "" FORCE)
set(HB_HAVE_CORETEXT        OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(harfbuzz)

# ── font_gen executable / WASM module ─────────────────────────────────────
add_executable(font_gen font_gen.c)

# MSVC C mode lacks C99 compound literals and designated initializers;
# compile as C++ there. Emscripten (clang-based) and GCC handle C99 fine.
if(MSVC)
    set_source_files_properties(font_gen.c PROPERTIES LANGUAGE CXX)
    target_compile_definitions(font_gen PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
endif()

target_link_libraries(font_gen PRIVATE freetype harfbuzz)

if(FONT_GEN_WASM)
    # ── Emscripten WASM target ─────────────────────────────────────────────
    set_target_properties(font_gen PROPERTIES SUFFIX ".js")
    target_link_options(font_gen PRIVATE
        -O2
        -sWASM=1
        -sMODULARIZE=1
        "-sEXPORT_NAME=FontGen"
        "-sEXPORTED_FUNCTIONS=[_malloc,_free,_font_gen_fnts_wasm,_font_gen_preview_wasm]"
        "-sEXPORTED_RUNTIME_METHODS=[HEAPU8]"
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=67108864
        "-sENVIRONMENT=worker"
        -sEXPORT_ES6=1
    )
else()
    # ── Native build ───────────────────────────────────────────────────────
    # math library on Linux/macOS
    if(UNIX)
        target_link_libraries(font_gen PRIVATE m)
    endif()
endif()
