# The native runtime: Skate 3's recompiled code plus our implementation of the console.
# Needs the generated C++ from scripts/recomp.ps1 (kept in the work folder, not the repo).
set(S3N_RECOMP_OUT "$ENV{LOCALAPPDATA}/Skate3Native/recomp/out" CACHE PATH "XenonRecomp output for Skate 3")
if(NOT EXISTS "${S3N_RECOMP_OUT}/ppc_recomp_shared.h")
    message(STATUS "No recompiled code in ${S3N_RECOMP_OUT}; skipping the runtime (run scripts/recomp.ps1)")
    return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Flags XenonRecomp output needs: it type-puns guest memory, and the vector code is
# written against SSE4/AVX intrinsics (Sandy Bridge has no FMA, so no fused rounding).
set(S3N_RECOMP_FLAGS /clang:-fno-strict-aliasing /clang:-march=sandybridge)

# The recompiled game: ~280 generated files, ~220 MB of C++. Compiles are memory-hungry,
# so Ninja runs at most 8 of them at once.
file(GLOB S3N_GUEST_SOURCES CONFIGURE_DEPENDS "${S3N_RECOMP_OUT}/ppc_recomp.*.cpp")
add_library(s3n_guest STATIC ${S3N_GUEST_SOURCES} "${S3N_RECOMP_OUT}/ppc_func_mapping.cpp")
target_include_directories(s3n_guest BEFORE PUBLIC "${S3N_RECOMP_OUT}")
target_link_libraries(s3n_guest PUBLIC XenonUtils)
target_compile_options(s3n_guest PRIVATE ${S3N_RECOMP_FLAGS} /bigobj /GS- /w)
set_property(GLOBAL APPEND PROPERTY JOB_POOLS guest_compile=8)
set_property(TARGET s3n_guest PROPERTY JOB_POOL_COMPILE guest_compile)

# One logging stub per import not implemented in kernel/.
file(GLOB S3N_KERNEL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/*.cpp")
set(S3N_IMPORT_STUBS "${CMAKE_CURRENT_BINARY_DIR}/import_stubs.cpp")
add_custom_command(OUTPUT "${S3N_IMPORT_STUBS}"
    COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/gen_import_stubs.py"
            "${S3N_RECOMP_OUT}/ppc_recomp_shared.h" "${S3N_IMPORT_STUBS}" "${CMAKE_CURRENT_SOURCE_DIR}/kernel"
    DEPENDS gen_import_stubs.py "${S3N_RECOMP_OUT}/ppc_recomp_shared.h" ${S3N_KERNEL_SOURCES}
    COMMENT "Generating import stubs")

file(GLOB S3N_GPU_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gpu/*.cpp")
add_executable(s3native main.cpp memory.cpp loader.cpp disc.cpp ${S3N_KERNEL_SOURCES} ${S3N_GPU_SOURCES}
    "${S3N_IMPORT_STUBS}")
target_include_directories(s3native PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(s3native PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(s3native PRIVATE s3n_guest s3n_common)
target_compile_options(s3native PRIVATE ${S3N_RECOMP_FLAGS})
