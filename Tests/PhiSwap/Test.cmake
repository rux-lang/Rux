include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

rux_register_error(1 "FibLoop(0) != 0   — phi swap corrupted loop variable")
rux_register_error(2 "FibLoop(1) != 1   — phi swap corrupted loop variable")
rux_register_error(3 "FibLoop(5) != 5   — phi swap corrupted loop variable")
rux_register_error(4 "FibLoop(10) != 55  — phi swap corrupted loop variable")
rux_register_error(5 "FibLoop(15) != 610 — phi swap corrupted loop variable")

rux_prepare_test("${TEST_DIR}" "phi_swap_test" target_binary)
rux_assert_executable("${target_binary}")
