# Smart Lamp Demo — LED driver
#
# Modbus TCP server (vendored libmodbus) + local animation at 20 Hz.
# The driver is a standalone process; it does not link FreeRTOS.

add_library(driver_core STATIC
    src/register_map.c
    src/sequence.c)

target_include_directories(driver_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(driver_core PRIVATE c_std_99)
target_compile_definitions(driver_core PUBLIC _POSIX_C_SOURCE=200809L)
target_link_libraries(driver_core PUBLIC Threads::Threads)

add_executable(driver
    src/main.c
    src/modbus_server.c)

target_link_libraries(driver PRIVATE driver_core modbus)
target_compile_features(driver PRIVATE c_std_99)
target_compile_definitions(driver PRIVATE _POSIX_C_SOURCE=200809L)
target_include_directories(driver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
