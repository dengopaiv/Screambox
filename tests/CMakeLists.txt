# The sample-exact tester's render driver. render.c is C and talks to the
# engine only through sb_api.h; sb_api_cpp.cpp implements that header over the
# C++ engine and file code - the same sources the program builds, nothing else
# (no UI, no audio device). tools/check_sound.py runs it.
add_executable(screambox_render
    render.c
    sb_api.h
    sb_api_cpp.cpp
    ${CMAKE_SOURCE_DIR}/src/CBEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/AcidFx.cpp
    ${CMAKE_SOURCE_DIR}/src/Distortion.cpp
    ${CMAKE_SOURCE_DIR}/src/IIR.cpp
    ${CMAKE_SOURCE_DIR}/src/dist.cpp
    ${CMAKE_SOURCE_DIR}/src/freq.cpp
    ${CMAKE_SOURCE_DIR}/src/RoqIO.cpp
    ${CMAKE_SOURCE_DIR}/src/CbPresetIO.cpp
)
target_include_directories(screambox_render PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(screambox_render PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
