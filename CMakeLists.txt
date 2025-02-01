cmake_minimum_required(VERSION 3.10)
project(bsdmon C)

set(CMAKE_C_STANDARD 11)

# Main executable
add_executable(${PROJECT_NAME} src/main.c)
