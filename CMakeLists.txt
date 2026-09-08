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

# Monocular depth estimation. ONNX Runtime is reached through its C API, so no
# C++ enters normalish itself; it is a prebuilt binary plus a model file that
# is downloaded separately. Second scoped exception to the C-and-raylib rule,
# agreed 2026-09-08 and recorded in PLAN.md.
#
# NuGet packages are plain zips and nuget.exe is not installed here, so they
# are fetched straight from the API. DOWNLOAD_NAME gives the .nupkg a .zip
# extension so FetchContent knows how to unpack it; neither package has a
# CMakeLists.txt, so MakeAvailable only populates.
set(ORT_VERSION 1.24.4)
set(DML_VERSION 1.15.4)

FetchContent_Declare(
    onnxruntime
    URL https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/${ORT_VERSION}
    DOWNLOAD_NAME onnxruntime-directml-${ORT_VERSION}.zip
)
FetchContent_MakeAvailable(onnxruntime)

# DirectML.dll is not in the ONNX Runtime package - that declares a dependency
# on this one instead. Windows does ship a DirectML.dll in System32, but not
# necessarily the version ONNX Runtime was built against, so ship this one.
FetchContent_Declare(
    directml
    URL https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/${DML_VERSION}
    DOWNLOAD_NAME directml-${DML_VERSION}.zip
)
FetchContent_MakeAvailable(directml)

set(ORT_INCLUDE_DIR ${onnxruntime_SOURCE_DIR}/build/native/include)
set(ORT_RUNTIME_DIR ${onnxruntime_SOURCE_DIR}/runtimes/win-x64/native)

add_executable(normalish src/main.c src/webcam.c src/vcamhost.c src/depth.c src/depthmodel.c)
# Media Foundation supplies webcam capture; it ships with Windows. mf.lib is
# needed for MFEnumDeviceSources specifically, which is not in mfplat.lib.
target_include_directories(normalish PRIVATE ${ORT_INCLUDE_DIR})
target_link_libraries(normalish PRIVATE raylib mf mfplat mfreadwrite mfuuid mfsensorgroup ole32
    ${ORT_RUNTIME_DIR}/onnxruntime.lib)

# onnxruntime.dll needs DirectML.dll beside it, and both need to be beside the
# executable rather than only in the fetch cache.
add_custom_command(TARGET normalish POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${ORT_RUNTIME_DIR}/onnxruntime.dll
        ${ORT_RUNTIME_DIR}/onnxruntime_providers_shared.dll
        ${directml_SOURCE_DIR}/bin/x64-win/DirectML.dll
        $<TARGET_FILE_DIR:normalish>
    COMMENT "Copying ONNX Runtime and DirectML next to normalish.exe")
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
    src/vcam/NormalishFrames.cpp
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
