cmake_minimum_required(VERSION 3.16)
project(websocket_echo_server LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBWEBSOCKETS REQUIRED IMPORTED_TARGET libwebsockets)

add_executable(echo_server_cpp echo_server.cpp)
target_compile_features(echo_server_cpp PRIVATE cxx_std_17)
target_compile_options(echo_server_cpp PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(echo_server_cpp PRIVATE PkgConfig::LIBWEBSOCKETS)
