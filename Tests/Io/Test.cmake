include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

rux_prepare_test("${TEST_DIR}" "io_test" target_binary)

rux_add_io_test(
        NAME io_test
        BINARY "${target_binary}"
        STDIN ""
        EXPECT "Hello from a Rux binary via I/O thunks!\n"
)