add_executable(smoke_host smoke_host.cpp)
target_compile_definitions(smoke_host PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_options(smoke_host PRIVATE -static -static-libgcc -static-libstdc++)
