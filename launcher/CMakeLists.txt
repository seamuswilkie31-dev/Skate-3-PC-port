add_executable(s3launcher WIN32 main.cpp)
target_link_libraries(s3launcher PRIVATE s3n_common imgui SDL3::SDL3-static)
