include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

rux_register_error(1  "float32 literal assignment failed")
rux_register_error(2  "float64 literal assignment failed")
rux_register_error(3  "default float literal is not float64")

rux_register_error(4  "float addition failed")
rux_register_error(5  "float subtraction failed")
rux_register_error(6  "float multiplication failed")
rux_register_error(7  "float division failed")
rux_register_error(8  "float exponentiation failed") # TODO: implement this
rux_register_error(9  "float remainder failed")

rux_register_error(10  "unary minus failed")

rux_register_error(11 "float compound += failed")
rux_register_error(12 "float compound -= failed")
rux_register_error(13 "float compound *= failed")
rux_register_error(14 "float compound /= failed")

rux_register_error(15 "float equality comparison failed")
rux_register_error(16 "float inequality comparison failed")
rux_register_error(17 "float less-than comparison failed")
rux_register_error(18 "float less-than-or-equal comparison failed")
rux_register_error(19 "float greater-than comparison failed")
rux_register_error(20 "float greater-than-or-equal comparison failed")

rux_register_error(21 "scientific notation 1.0e3 failed")
rux_register_error(22 "scientific notation 1.5e2 failed")
rux_register_error(23 "scientific notation 1.0e-2 failed")

rux_register_error(24 "int32 -> float64 conversion failed")
rux_register_error(25 "float64 -> int32 truncation failed")
rux_register_error(26 "negative float64 -> int32 truncation failed")

rux_register_error(27 "float32 -> float64 widening failed")
rux_register_error(28 "float64 -> float32 narrowing failed")

rux_register_error(29 "float constant folding failed for A")
rux_register_error(30 "float constant folding failed for B")

rux_register_error(31 "float operator precedence failed")
rux_register_error(32 "float parenthesized precedence failed")

rux_register_error(33 "negative zero comparison failed")

rux_register_error(34 "positive infinity generation failed")
rux_register_error(35 "negative infinity generation failed")

rux_register_error(36 "NaN equality rule violated")
rux_register_error(37 "NaN inequality rule violated")

rux_register_error(38 "infinity propagation failed")

rux_register_error(39 "float64 exact integer representation failed")

rux_prepare_test("${TEST_DIR}" "floating_point_types_test" target_binary)

rux_assert_executable("${target_binary}")
