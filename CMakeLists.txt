cmake_minimum_required(VERSION 3.20)
# Screambox - formerly Creakbox, renamed 2026-11-06. Built on CreakBox (2002) by
# Frank Reitz (Bass Assessor) and its 2004 VST source; see README "Lineage".
project(Screambox LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static C/C++ runtime: Screambox ships as one exe, and with the default DLL
# runtime it would not start on a machine without the Visual C++
# redistributable (MSVCP140.dll / VCRUNTIME140.dll). Set before any target.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8 /Zc:__cplusplus)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(SCREAMBOX_SOURCES
    src/WinMain.cpp
    src/MainWindow.cpp
    src/AudioEngine.cpp
    src/CBEngine.cpp
    src/CbPresetIO.cpp
    src/RoqIO.cpp
    src/AcidFx.cpp
    src/Distortion.cpp
    # Original DSP reused verbatim
    src/IIR.cpp
    src/dist.cpp
    src/freq.cpp
)

set(SCREAMBOX_HEADERS
    src/MainWindow.h
    src/AudioEngine.h
    src/CBEngine.h
    src/CbPresetIO.h
    src/RoqIO.h
    src/Distortion.h
    src/IIR.h
    src/dist.h
    src/freq.h
    src/core.h
    src/version.h
)

add_executable(Screambox WIN32
    ${SCREAMBOX_SOURCES}
    ${SCREAMBOX_HEADERS}
    src/resource.rc
)

target_include_directories(Screambox PRIVATE src)
target_link_libraries(Screambox PRIVATE
    ole32
    uuid
    comctl32
    comdlg32
    shlwapi
    user32
    gdi32
    shell32
    advapi32
)

if(MSVC)
    set_target_properties(Screambox PROPERTIES
        LINK_FLAGS "/MANIFESTUAC:\"level='asInvoker' uiAccess='false'\" /SUBSYSTEM:WINDOWS"
    )
endif()

# The sample-exact tester (tools/check_sound.py). Not part of the release.
option(SCREAMBOX_TESTER "Build the screambox_render test driver" ON)
if(SCREAMBOX_TESTER)
    add_subdirectory(tests)
endif()
