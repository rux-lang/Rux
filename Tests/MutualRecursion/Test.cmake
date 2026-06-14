include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

foreach (i RANGE 0 100)
    math(EXPR even_code "2 * ${i} + 1")
    math(EXPR odd_code  "2 * ${i} + 2")

    rux_register_error(${even_code} "IsEven(${i}) failed")
    rux_register_error(${odd_code}  "IsOdd(${i}) failed")
endforeach ()

rux_prepare_test("${TEST_DIR}" "mutual_recursion_test" target_binary)

rux_assert_executable("${target_binary}")