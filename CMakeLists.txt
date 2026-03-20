cmake_minimum_required(VERSION 3.16)
project(spi_lib VERSION 1.0.0 LANGUAGES C)
set(CMAKE_C_STANDARD 11)

add_library(spi src/spi.c drivers/spi_devices.c)
target_include_directories(spi PUBLIC include drivers)

add_executable(example_all_devices examples/example_all_devices.c)
target_link_libraries(example_all_devices PRIVATE spi m)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(spi PRIVATE -Wall -Wextra -Wpedantic)
endif()
