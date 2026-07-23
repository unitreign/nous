cmake_minimum_required(VERSION 3.16)
project(nous_tests CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Allow older subprojects to configure on CMake 4.x
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../lib/nous)

# miniz (vendored) — needed transitively by BookIndex -> Book -> ZipReader.
set(MINIZ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniz)
add_library(miniz STATIC
    ${MINIZ_DIR}/miniz.c
    ${MINIZ_DIR}/miniz_tdef.c
    ${MINIZ_DIR}/miniz_tinfl.c
    ${MINIZ_DIR}/miniz_zip.c
)
target_include_directories(miniz PUBLIC ${MINIZ_DIR})

add_library(nous_content STATIC
    ${LIB_DIR}/content/HtmlEntities.cpp
    ${LIB_DIR}/content/ZipReader.cpp
    ${LIB_DIR}/content/XmlReader.cpp
    ${LIB_DIR}/content/CssParser.cpp
    ${LIB_DIR}/content/EpubParser.cpp
    ${LIB_DIR}/content/ImageDecoder.cpp
    ${LIB_DIR}/content/JpegDecoder.cpp
    ${LIB_DIR}/content/PngDecoder.cpp
    ${LIB_DIR}/content/BmpSleepConverter.cpp
    ${LIB_DIR}/content/Book.cpp
    ${LIB_DIR}/content/BookIndex.cpp
    ${LIB_DIR}/content/TextLayout.cpp
    ${LIB_DIR}/content/hyphenation/Hyphenation.cpp
    ${LIB_DIR}/content/mrb/MrbWriter.cpp
    ${LIB_DIR}/content/mrb/MrbReader.cpp
    ${LIB_DIR}/content/mrb/MrbConverter.cpp
)
target_include_directories(nous_content PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../lib)
target_link_libraries(nous_content PUBLIC miniz)

# Fast unit tests — no EPUB fixtures, no display, runs in milliseconds.
add_executable(unit_tests
    unit/ReadingActivityTrackerTest.cpp
    unit/BookIndexStatsEpochTest.cpp
)
target_link_libraries(unit_tests PRIVATE nous_content gtest gtest_main)
