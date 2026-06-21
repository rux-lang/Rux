include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

rux_register_error(1 "24-byte struct return corrupted at .a (Big::New)")
rux_register_error(2 "24-byte struct return corrupted at .b (Big::New)")
rux_register_error(3 "24-byte struct return corrupted at .c (Big::New)")
rux_register_error(4 "24-byte struct return with args corrupted at .a (Big::WithArgs)")
rux_register_error(5 "24-byte struct return with args corrupted at .b (Big::WithArgs)")
rux_register_error(6 "24-byte struct return with args corrupted at .c (Big::WithArgs)")
rux_register_error(7 "16-byte struct return regressed at .a (Small::New)")
rux_register_error(8 "16-byte struct return regressed at .b (Small::New)")

rux_prepare_test("${TEST_DIR}" "struct_return_test" target_binary)

rux_assert_executable("${target_binary}")
