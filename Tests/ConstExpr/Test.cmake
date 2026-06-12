# Verifies that constant integer EXPRESSIONS (not just bare literals) coerce to
# a sized integer type when their folded value fits, e.g. the documented
# `10 + 2 * (5 - 3)` as int32. Before the fix only a bare literal coerced, so a
# literal arithmetic expression assigned to a sized int was rejected with
# "cannot assign 'int' to 'int32'".

include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

rux_register_error(1 "10 + 2 * (5 - 3) should evaluate to 14 (int32 coercion)")
rux_register_error(2 "100 + 27 should evaluate to 127 (int8 boundary coercion)")
rux_register_error(3 "200 + 55 should evaluate to 255 (uint8 boundary coercion)")
rux_register_error(4 "1 << 10 should evaluate to 1024 (int16 shift constant fold)")
rux_register_error(5 "0xF0 | 0x0F should evaluate to 255 (uint8 bitwise constant fold)")
rux_register_error(6 "(3 + 4) * (10 - 2) should evaluate to 56 (int32 arithmetic fold)")

rux_prepare_test("${TEST_DIR}" "const_expr_test" target_binary)

rux_assert_executable("${target_binary}")