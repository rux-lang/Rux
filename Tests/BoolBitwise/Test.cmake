include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

rux_register_error(1 "bool8 AND (&) failed")
rux_register_error(2 "bool8 OR (|) failed")
rux_register_error(3 "bool8 XOR (^) failed")
rux_register_error(4 "bool8 Negation (~) failed")
rux_register_error(5 "bool8 Negation (~b8) failed")

rux_register_error(6 "bool8 Truth Table (true & true) failed")
rux_register_error(7 "bool8 Truth Table (true & false) failed")
rux_register_error(8 "bool8 Truth Table (false & true) failed")
rux_register_error(9 "bool8 Truth Table (false & false) failed")

rux_register_error(10 "bool16 AND (&) failed")
rux_register_error(11 "bool16 OR (|) failed")
rux_register_error(12 "bool16 XOR (^) failed")
rux_register_error(13 "bool16 Negation (~) failed")
rux_register_error(14 "bool16 self XOR failed")

rux_register_error(15 "bool32 AND (&) failed")
rux_register_error(16 "bool32 OR (|) failed")
rux_register_error(17 "bool32 XOR (^) failed")
rux_register_error(18 "bool32 Negation (~) failed")

rux_register_error(19 "De Morgan's Law sanity check failed")

rux_register_error(20 "Mixed width bool8 & bool16 failed")
rux_register_error(21 "Mixed width bool32 | bool8 failed")

rux_prepare_test("${TEST_DIR}" "bool_bitwise_test" target_binary)

rux_assert_executable("${target_binary}")