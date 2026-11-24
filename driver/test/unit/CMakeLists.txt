# Smart Lamp Demo — driver/test/unit

add_executable(test_register_map test_register_map.c)
target_link_libraries(test_register_map PRIVATE driver_core unity)
target_compile_features(test_register_map PRIVATE c_std_99)
add_test(NAME test_register_map COMMAND test_register_map)

add_executable(test_sequence test_sequence.c)
target_link_libraries(test_sequence PRIVATE driver_core unity)
target_compile_features(test_sequence PRIVATE c_std_99)
add_test(NAME test_sequence COMMAND test_sequence)
