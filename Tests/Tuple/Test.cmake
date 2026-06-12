include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

rux_register_error(1  "Size mismatch for tuple (int8, int32, int8) - Padding/alignment issue")
rux_register_error(2  "Size mismatch for tuple (int32, int8) - Padding/alignment issue")
rux_register_error(3  "Size mismatch for tuple (int8, int16, int32) - Padding/alignment issue")
rux_register_error(4  "Size mismatch for tuple (int64, int8, int64) - Padding/alignment issue")
rux_register_error(5  "Size mismatch for tuple (float64, int32, float64) - Padding/alignment issue")
rux_register_error(6  "Size mismatch for uniform tuple (int32, int32, int32)")
rux_register_error(7  "Size mismatch for packed byte tuple (int8, int8, int8)")

rux_register_error(8  "Value corruption at t1.0 (int8)")
rux_register_error(9  "Value corruption at t1.1 (int32)")
rux_register_error(10 "Value corruption at t1.2 (int8)")

rux_register_error(11 "Value corruption at t2.0 (int32)")
rux_register_error(12 "Value corruption at t2.1 (int8)")

rux_register_error(13 "Value corruption at t3.0 (int8)")
rux_register_error(14 "Value corruption at t3.1 (int16)")
rux_register_error(15 "Value corruption at t3.2 (int32)")

rux_register_error(16 "Value corruption at t4.0 (int64)")
rux_register_error(17 "Value corruption at t4.1 (int8)")
rux_register_error(18 "Value corruption at t4.2 (int64)")

rux_register_error(19 "Value corruption at t5.0 (float64)")
rux_register_error(20 "Value corruption at t5.1 (int32)")
rux_register_error(21 "Value corruption at t5.2 (float64)")

rux_register_error(22 "Pattern matching failed to extract variable 'a' (int16)")
rux_register_error(23 "Pattern matching failed to extract variable 'b' (int32)")
rux_register_error(24 "Pattern matching failed to extract variable 'c' (int64)")

rux_register_error(25 "Nested access failed: nested.0.0 (int8) is invalid")
rux_register_error(26 "Nested access failed: nested.0.1 (int16) is invalid")
rux_register_error(27 "Nested access failed: nested.1.0 (int32) is invalid")
rux_register_error(28 "Nested access failed: nested.1.1 (int64) is invalid")

rux_prepare_test("${TEST_DIR}" "tuple_test" target_binary)

rux_assert_executable("${target_binary}")