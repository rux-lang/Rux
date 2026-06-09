# Verifies that hex, binary, and octal integer literals accept a type suffix
# (e.g. 0xFFu), the same way decimal literals already do. Before the fix the
# lexer treated the suffix letter as an invalid digit for the base and errored
# ("invalid digit 'u' for base 16"). Also checks '_' digit separators.

include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

rux_register_error(1 "2 ** 10 != 1024")
rux_register_error(2 "3 ** 4 != 81")
rux_register_error(3 "10 ** 3 != 1000")
rux_register_error(4 "7 ** 1 != 7")
rux_register_error(5 "5 ** 0 != 1")
rux_register_error(6 "2 ** 0 != 1")

rux_prepare_test("${TEST_DIR}" "pow_test" target_binary)

rux_assert_executable("${target_binary}")