cmake_minimum_required(VERSION 3.25)

project(LipSyncAviUtl1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(lipsync_analyzer STATIC
    src/japanese_reading.cpp
    src/lipsync_analyzer.cpp
    src/wav_audio_source.cpp
    third_party/ooura/fftsg.c
)
target_include_directories(lipsync_analyzer PUBLIC src)
target_compile_options(lipsync_analyzer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /W4 /permissive->
    $<$<C_COMPILER_ID:MSVC>:/W3>
)

add_library(LipSyncAviUtl1 SHARED src/module.cpp)
target_include_directories(LipSyncAviUtl1 PRIVATE
    third_party/aviutl2_sdk/include/aviutl2_sdk
)
target_link_libraries(LipSyncAviUtl1 PRIVATE lipsync_analyzer ole32 oleaut32)
target_compile_definitions(LipSyncAviUtl1 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(LipSyncAviUtl1 PRIVATE /utf-8 /W4 /permissive-)
set_target_properties(LipSyncAviUtl1 PROPERTIES
    PREFIX ""
    SUFFIX ".mod2"
)

enable_testing()
add_executable(LipSyncAviUtl1Tests tests/lipsync_analyzer_tests.cpp)
target_link_libraries(LipSyncAviUtl1Tests PRIVATE lipsync_analyzer ole32 oleaut32)
target_compile_options(LipSyncAviUtl1Tests PRIVATE /utf-8 /W4 /permissive-)
add_test(NAME LipSyncAviUtl1Tests COMMAND LipSyncAviUtl1Tests)

add_executable(LipSyncAviUtl1WavProbe tools/analyze_wav.cpp)
target_link_libraries(LipSyncAviUtl1WavProbe PRIVATE lipsync_analyzer)
target_compile_definitions(LipSyncAviUtl1WavProbe PRIVATE NOMINMAX)
target_compile_options(LipSyncAviUtl1WavProbe PRIVATE /utf-8 /W4 /permissive-)

add_executable(LipSyncAdaptiveWavProbe tools/analyze_adaptive.cpp)
target_link_libraries(LipSyncAdaptiveWavProbe PRIVATE lipsync_analyzer)
target_compile_definitions(LipSyncAdaptiveWavProbe PRIVATE NOMINMAX)
target_compile_options(LipSyncAdaptiveWavProbe PRIVATE /utf-8 /W4 /permissive-)

add_executable(JapaneseReadingProbe tools/analyze_japanese_reading.cpp)
target_include_directories(JapaneseReadingProbe PRIVATE src)
target_compile_definitions(JapaneseReadingProbe PRIVATE UNICODE _UNICODE)
target_compile_options(JapaneseReadingProbe PRIVATE /utf-8 /W4 /permissive-)
target_link_libraries(JapaneseReadingProbe PRIVATE lipsync_analyzer ole32 oleaut32)
