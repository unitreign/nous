cmake_minimum_required(VERSION 3.16)

# Use /Z7 (embedded debug info per .obj) instead of shared /Zi .pdb to avoid
# C1041 PDB lock errors from parallel MSVC compilation.
# CMP0141 must be set before project() to take effect.
if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
endif()

project(microreader_desktop C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# lib/nous/version.h is checked in; tools/generate_version.py still targets the old lib/microreader path
# ---------------------------------------------------------------------------
# SDL2 - use a prebuilt SDL2 if SDL2_ROOT is given (folder with include/ and
# lib/ or a DLL), otherwise fetch and build it statically.
# ---------------------------------------------------------------------------
set(SDL2_ROOT "" CACHE PATH "Prebuilt SDL2 folder (include/ + lib/)")
if(SDL2_ROOT)
  add_library(sdl2_prebuilt INTERFACE)
  target_include_directories(sdl2_prebuilt INTERFACE ${SDL2_ROOT}/include ${SDL2_ROOT}/include/SDL2)
  target_link_libraries(sdl2_prebuilt INTERFACE ${SDL2_ROOT}/lib/SDL2.dll)
  set(SDL2_TARGET sdl2_prebuilt)
else()
  include(FetchContent)
  FetchContent_Declare(
      SDL2
      GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
      GIT_TAG        release-2.32.10
      GIT_SHALLOW    TRUE
  )
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON  CACHE BOOL "" FORCE)
  set(SDL_TEST   OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL2)
  set(SDL2_TARGET SDL2::SDL2-static)
endif()

# Screen size override for testing other devices, e.g. -DNOUS_SCREEN_W=790 -DNOUS_SCREEN_H=528
set(NOUS_SCREEN_W "" CACHE STRING "App-space width override (landscape)")
set(NOUS_SCREEN_H "" CACHE STRING "App-space height override (landscape)")

# miniz (vendored in third_party/miniz/ — all .c files compiled as separate TUs)
set(MINIZ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/miniz)
add_library(miniz STATIC
    ${MINIZ_DIR}/miniz.c
    ${MINIZ_DIR}/miniz_tdef.c
    ${MINIZ_DIR}/miniz_tinfl.c
    ${MINIZ_DIR}/miniz_zip.c
)
target_include_directories(miniz PUBLIC ${MINIZ_DIR})

# ---------------------------------------------------------------------------
# nous library (everything under lib/nous)
# ---------------------------------------------------------------------------
# Same file list the firmware builds (platforms/esp32/CMakeLists.txt), so the
# desktop build never drifts from it again.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../esp32/CMakeLists.txt _esp_lines REGEX "LIB}/.*cpp")
set(NOUS_SOURCES "")
foreach(_l ${_esp_lines})
  string(REGEX REPLACE ".*LIB}/([^ )]+cpp).*" "\\1" _f "${_l}")
  list(APPEND NOUS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/nous/${_f})
endforeach()
add_library(microreader_core STATIC ${NOUS_SOURCES})
target_include_directories(microreader_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../lib)
target_link_libraries(microreader_core PUBLIC miniz)
if(NOUS_SCREEN_W AND NOUS_SCREEN_H)
  target_compile_definitions(microreader_core PUBLIC NOUS_SCREEN_W=${NOUS_SCREEN_W} NOUS_SCREEN_H=${NOUS_SCREEN_H})
endif()

# ---------------------------------------------------------------------------
# Desktop executable
# ---------------------------------------------------------------------------
add_executable(microreader_desktop
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)
target_link_libraries(microreader_desktop PRIVATE
    microreader_core
    ${SDL2_TARGET}
)
# Statically link MinGW runtime so the exe runs without libstdc++/libgcc DLLs.
if(MINGW)
  target_link_options(microreader_desktop PRIVATE -static-libgcc -static-libstdc++)
  target_link_libraries(microreader_desktop PRIVATE -Wl,-Bstatic winpthread -Wl,-Bdynamic)
endif()
# Prevent SDL_main.h from renaming main() so we keep a plain console entry point.
target_compile_definitions(microreader_desktop PRIVATE SDL_MAIN_HANDLED)
# Generate a header with the repo-root sd/ path (avoids MSVC /D drive-letter colon issue).
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(MICROREADER_SD_DIR "${REPO_ROOT}/sd" CACHE PATH "Folder used as the emulated SD card")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/desktop_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/desktop_config.h
    @ONLY
)
target_include_directories(microreader_desktop PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
