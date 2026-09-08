cmake_minimum_required(VERSION 3.21)
# CXX is only for the virtual camera media source DLL, which has to implement
# COM interfaces. Everything else is C.
project(normalish C CXX)

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
# Windows Implementation Library: header-only, needed by the adopted virtual
# camera source. Pinned to the version that source was built against.
FetchContent_Declare(
    wil
    GIT_REPOSITORY https://github.com/microsoft/wil.git
    GIT_TAG v1.0.260126.7
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR include
)
FetchContent_MakeAvailable(wil)

# The virtual camera media source. Loaded by the Windows Camera Frame Server,
# not by normalish itself, so it is a separate DLL rather than linked in.
#
# Adopted from smourier/VCamSample (MIT) rather than written from scratch: see
# PLAN.md. Eight rounds of mirroring its behaviour onto a hand-written source
# failed to produce frames, while the original streams correctly here.
add_library(normalish_vcam SHARED
    src/vcam/dllmain.cpp
    src/vcam/Activator.cpp
    src/vcam/MediaSource.cpp
    src/vcam/MediaStream.cpp
    src/vcam/FrameGenerator.cpp
    src/vcam/MFTools.cpp
    src/vcam/Tools.cpp
    src/vcam/EnumNames.cpp
    src/vcam/WinTrace.cpp
    src/vcam/vcam.def)
set_target_properties(normalish_vcam PROPERTIES CXX_STANDARD 20)
# The adopted source assumes a Unicode build, as Visual Studio projects do by
# default. Without these, Win32 calls resolve to their ...A variants and every
# wide string argument fails to compile.
target_compile_definitions(normalish_vcam PRIVATE UNICODE _UNICODE)
target_include_directories(normalish_vcam PRIVATE
    src/vcam
    ${wil_SOURCE_DIR}/include)
# winrt/base.h comes from the Windows SDK, already on the include path.
target_link_libraries(normalish_vcam PRIVATE
    mfplat mfuuid mf mfsensorgroup mfreadwrite
    d2d1 dwrite windowscodecs d3d11 dxgi
    ole32 oleaut32 propsys advapi32)

target_compile_definitions(normalish PRIVATE
    SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
