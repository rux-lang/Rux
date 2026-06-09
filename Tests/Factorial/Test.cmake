include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

set(expected_values
        1  # 0!
        1  # 1!
        2  # 2!
        6  # 3!
        24 # 4!
        120 # 5!
        720 # 6!
        5040 # 7!
        40320 # 8!
        362880 # 9!
        3628800 # 10!
        39916800 # 11!
        479001600 # 12!
)

set(i 0)

foreach (exp IN LISTS expected_values)
    math(EXPR rec_code "2 * ${i} + 1")
    math(EXPR iter_code "2 * ${i} + 2")

    rux_register_error(${rec_code} "FactRec(${i}) != ${exp}")
    rux_register_error(${iter_code} "FactIter(${i}) != ${exp}")

    math(EXPR i "${i} + 1")
endforeach ()

foreach (i RANGE 0 12)
    math(EXPR code "27 + ${i}")
    rux_register_error(${code} "FactRec(${i}) != FactIter(${i})")
endforeach ()

foreach (i RANGE 0 4)
    math(EXPR code "39 + ${i}")
    rux_register_error(${code} "Expression mismatch at i=${i}")
endforeach ()

foreach (i RANGE 0 10)
    math(EXPR rec_code  "49 + ${i}")
    math(EXPR iter_code "59 + ${i}")

    rux_register_error(${rec_code}  "FactRec(${i}) not deterministic")
    rux_register_error(${iter_code} "FactIter(${i}) not deterministic")
endforeach ()

rux_prepare_test("${TEST_DIR}" "factorial_test" target_binary)

rux_assert_executable("${target_binary}")
