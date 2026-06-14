include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

rux_register_error(1  "true literal failed")
rux_register_error(2  "false literal failed")

rux_register_error(3  "bool alias assignment failed")
rux_register_error(4  "bool8 assignment failed")
rux_register_error(5  "bool32 assignment failed")

rux_register_error(6  "true as int32 != 1")
rux_register_error(7  "false as int32 != 0")

rux_register_error(8  "1 as bool != true")
rux_register_error(9 "0 as bool != false")

rux_register_error(10 "bool8 -> bool32 cast failed")
rux_register_error(11 "bool32 -> bool8 cast failed")

rux_register_error(12 "logical AND failed")
rux_register_error(13 "logical OR failed")
rux_register_error(14 "logical NOT failed")

rux_register_error(15 "bitwise AND failed")
rux_register_error(16 "bitwise OR failed")
rux_register_error(17 "bitwise XOR failed")
rux_register_error(18 "bitwise NOT failed")

rux_register_error(19 "equality failed")
rux_register_error(20 "inequality failed")

rux_register_error(21 "true -> uint64 cast failed")
rux_register_error(22 "false -> uint64 cast failed")

rux_register_error(23 "bool8 AND truth table TT failed")
rux_register_error(24 "bool8 AND truth table TF failed")
rux_register_error(25 "bool8 AND truth table FT failed")
rux_register_error(26 "bool8 AND truth table FF failed")

rux_register_error(26 "bool16 XOR self failed")

rux_register_error(27 "De Morgan law failed")

rux_register_error(28 "mixed bool8/bool16 AND failed")
rux_register_error(29 "mixed bool32/bool8 OR failed")

rux_prepare_test("${TEST_DIR}" "boolean_types_test" target_binary)

rux_assert_executable("${target_binary}")