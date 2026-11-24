# Smart Lamp Demo — firmware/test
#
# Unit tests with Unity + CTest. The `unity` target is defined in
# third_party/CMakeLists.txt (shared with the driver tests).
#
# Usage:
#   cmake -B build-test -G Ninja -DBUILD_TESTS=ON
#   cmake --build build-test
#   ctest --test-dir build-test --output-on-failure

add_subdirectory(unit)

# Integration: real supervisor process + TCP channel (fake bootloader).
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(NAME test_supervisor_control
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_supervisor_control.py"
        "$<TARGET_FILE:supervisor>")

set_tests_properties(test_supervisor_control PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    TIMEOUT 30)
