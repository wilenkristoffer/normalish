cmake_minimum_required(VERSION 3.21)
project(normalish C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

# raylib build options: we only want the library itself.
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(raylib)

# raygui is a single-header GUI library by raylib's author. SOURCE_SUBDIR points
# at a directory with no CMakeLists.txt so FetchContent only downloads it rather
# than trying to build raygui's own targets.
FetchContent_Declare(
    raygui
    GIT_REPOSITORY https://github.com/raysan5/raygui.git
    GIT_TAG 4.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR src
)
FetchContent_MakeAvailable(raygui)

add_executable(normalish src/main.c src/webcam.c)
# Media Foundation supplies webcam capture; it ships with Windows. mf.lib is
# needed for MFEnumDeviceSources specifically, which is not in mfplat.lib.
target_link_libraries(normalish PRIVATE raylib mf mfplat mfreadwrite mfuuid ole32)
target_include_directories(normalish PRIVATE ${raygui_SOURCE_DIR}/src)

# Load shaders straight from the source tree so the in-app reload key picks up
# edits without a rebuild.
target_compile_definitions(normalish PRIVATE
    SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
