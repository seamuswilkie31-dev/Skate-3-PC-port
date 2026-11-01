# Our recompilation tooling, built on XenonRecomp's generic XEX loader.
add_subdirectory(${S3N_XENONRECOMP_DIR} xenonrecomp EXCLUDE_FROM_ALL)

add_executable(xexscan tools/xexscan.cpp)
target_link_libraries(xexscan PRIVATE XenonUtils)
target_compile_definitions(xexscan PRIVATE NOMINMAX)  # XenonUtils headers pull in windows.h
