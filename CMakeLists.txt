cmake_minimum_required(VERSION 3.10)
project(rawsendmpeg2ts C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(rawsendmpeg2ts rawsendmpeg2ts.c)
target_compile_options(rawsendmpeg2ts PRIVATE -O2 -Wall -Wextra)
