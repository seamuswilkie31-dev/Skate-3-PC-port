add_executable(s3n_selftest selftest.cpp)
target_link_libraries(s3n_selftest PRIVATE s3n_common)
