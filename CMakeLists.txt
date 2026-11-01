cmake_minimum_required(VERSION 3.25)
project(Skate3Native VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Static CRT everywhere: XenonRecomp's libraries are built that way and the runtime links them.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.2.10.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.8.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(SDL3 imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

add_subdirectory(common)
add_subdirectory(launcher)
add_subdirectory(tests)

# Recompilation tools need a XenonRecomp checkout (scripts/setup_tools.ps1 makes one).
set(S3N_XENONRECOMP_DIR "$ENV{LOCALAPPDATA}/Skate3Native/tools/XenonRecomp" CACHE PATH "XenonRecomp checkout")
if(EXISTS "${S3N_XENONRECOMP_DIR}/CMakeLists.txt")
    add_subdirectory(recomp)
    add_subdirectory(runtime)
else()
    message(STATUS "XenonRecomp not found at ${S3N_XENONRECOMP_DIR}; skipping recomp tools and runtime")
endif()
