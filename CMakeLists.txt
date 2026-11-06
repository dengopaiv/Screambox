cmake_minimum_required(VERSION 3.20)
project(Creakbox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8 /Zc:__cplusplus)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(CREAKBOX_SOURCES
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

set(CREAKBOX_HEADERS
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
)

add_executable(Creakbox WIN32
    ${CREAKBOX_SOURCES}
    ${CREAKBOX_HEADERS}
    src/resource.rc
)

target_include_directories(Creakbox PRIVATE src)
target_link_libraries(Creakbox PRIVATE
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
    set_target_properties(Creakbox PROPERTIES
        LINK_FLAGS "/MANIFESTUAC:\"level='asInvoker' uiAccess='false'\" /SUBSYSTEM:WINDOWS"
    )
endif()
