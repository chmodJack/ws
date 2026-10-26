cmake_minimum_required(VERSION 3.16)
project(lws_c_client C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LWS REQUIRED IMPORTED_TARGET libwebsockets)

add_library(ws_client STATIC ws_client.c)
target_compile_options(ws_client PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ws_client PUBLIC PkgConfig::LWS Threads::Threads)
target_include_directories(ws_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(lws_client websocket_client.c)
target_compile_options(lws_client PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(lws_client PRIVATE ws_client)
