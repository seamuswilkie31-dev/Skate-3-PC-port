add_library(s3n_common STATIC
    config.cpp
    game_source.cpp
    net_adapters.cpp
    stfs.cpp
    xdvdfs.cpp
    xex_info.cpp)
target_include_directories(s3n_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_compile_definitions(s3n_common PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(s3n_common PUBLIC iphlpapi ws2_32)
endif()
