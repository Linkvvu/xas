cmake_minimum_required(VERSION 3.16)
project(xas VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(asio CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# Library target
add_library(xas STATIC
    src/TcpServer.cpp
    src/TcpSession.cpp
)
target_include_directories(xas PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(xas PUBLIC
    asio::asio
    spdlog::spdlog
)

add_subdirectory(tests)
add_subdirectory(examples/echo)
