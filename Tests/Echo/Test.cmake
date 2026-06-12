include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

rux_prepare_test("${TEST_DIR}" "echo_test" target_binary)

rux_add_io_test(
        NAME echo_test
        BINARY "${target_binary}"
        STDIN "some dummy input data to trigger readfile"
        EXPECT "read stdin OK\n"
)