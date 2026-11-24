# Smart Lamp Demo — root CMake
#
# Build entry point. The POSIX firmware (FreeRTOS) lives in
# firmware/; here we only pin the C standard and delegate the rest.
#
# Usage:
#   cmake -B build -G Ninja
#   cmake --build build
#   ./build/bin/firmware

cmake_minimum_required(VERSION 3.16)

project(smartlamp
    VERSION 0.1.0
    DESCRIPTION "Smart Lamp Demo — firmware POSIX, bootloader y driver LED"
    LANGUAGES C)

# --- Compilation standard ----------------------------------------------------
# Strict C99 (no GNU extensions) to stay close to MISRA C.
# Note: gcc 13 defaults to gnu17, hence the explicit setting.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# --- Options -----------------------------------------------------------------
option(BUILD_TESTS "Compila y registra los tests unitarios (Unity/CMock)" OFF)

if(BUILD_TESTS)
    enable_testing()
endif()

# --- Subprojects -------------------------------------------------------------
# Binaries (firmware, supervisor, driver, tests) in build/bin.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# pthread: FreeRTOS POSIX port and driver.
find_package(Threads REQUIRED)

add_subdirectory(third_party)
add_subdirectory(firmware)
add_subdirectory(driver)
