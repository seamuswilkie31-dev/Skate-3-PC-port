# FFmpeg's WMA Pro decoder, which the runtime's XMA decoder (runtime/apu/xma.cpp) feeds one XMA
# frame at a time: libavutil and the part of libavcodec it needs, compiled from the release
# source. FFmpeg builds with make and a configure script; instead, config/ and include/ hold
# that configure's result for this toolchain (clang-cl, x86-64, only the wmapro decoder) and
# sources.cmake the files its makefiles would compile. scripts/ffmpeg_config.ps1 regenerates
# them for another release. LGPL 2.1 or later: nothing GPL is enabled.
#
# The configuration has no threading, so FFmpeg's one-time table setup isn't thread safe:
# the runtime makes all its FFmpeg calls under one lock.

include(FetchContent)
FetchContent_Declare(ffmpeg
    URL https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz
    URL_HASH SHA256=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(ffmpeg)

include(sources.cmake)

# configure's CPPFLAGS for this toolchain, plus what FFmpeg's makefiles add to every object.
set(S3N_FFMPEG_DEFINITIONS HAVE_AV_CONFIG_H _ISOC11_SOURCE _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE
    WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
    _WIN32_WINNT=0x0600 PIC)

foreach(library avutil avcodec)
    string(TOUPPER ${library} upper)
    list(TRANSFORM FFMPEG_${upper}_SOURCES PREPEND "${ffmpeg_SOURCE_DIR}/")
    add_library(ffmpeg_${library} STATIC ${FFMPEG_${upper}_SOURCES})
    target_include_directories(ffmpeg_${library} BEFORE
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/config" "${ffmpeg_SOURCE_DIR}/compat/stdbit"
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${ffmpeg_SOURCE_DIR}")
    target_compile_definitions(ffmpeg_${library} PRIVATE ${S3N_FFMPEG_DEFINITIONS} BUILDING_${library})
    set_target_properties(ffmpeg_${library} PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    target_compile_options(ffmpeg_${library} PRIVATE /utf-8 /w)
endforeach()
target_link_libraries(ffmpeg_avutil PUBLIC user32 bcrypt)
target_link_libraries(ffmpeg_avcodec PUBLIC ffmpeg_avutil ole32)
add_library(FFmpeg::avcodec ALIAS ffmpeg_avcodec)
