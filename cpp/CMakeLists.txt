cmake_minimum_required(VERSION 3.16)
project(lws_c_client C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED IMPORTED_TARGET libwebsockets)

add_executable(lws_client websocket_client.c)
target_compile_options(lws_client PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(lws_client PRIVATE PkgConfig::LWS)
