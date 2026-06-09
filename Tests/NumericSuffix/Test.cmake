# Verifies that hex, binary, and octal integer literals accept a type suffix
# (e.g. 0xFFu), the same way decimal literals already do. Before the fix the
# lexer treated the suffix letter as an invalid digit for the base and errored
# ("invalid digit 'u' for base 16"). Also checks '_' digit separators.

include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

rux_register_error(1 "0xFFu != 255")
rux_register_error(2 "0b1010u != 10")
rux_register_error(3 "0o17i != 15")
rux_register_error(4 "0xDE_AD != 57005")
rux_register_error(5 "0b1111_1111 != 255")
rux_register_error(6 "0xFF != 255")

rux_prepare_test("${TEST_DIR}" "numeric_suffix_test" target_binary)

rux_assert_executable("${target_binary}")