include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

rux_prepare_test("${TEST_DIR}" "bare_impl_syntax" target_binary)

execute_process(
        COMMAND "${target_binary}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE raw_stdout
        ERROR_VARIABLE raw_stderr
        TIMEOUT 5
)

if (NOT rc EQUAL 0)
    message(FATAL_ERROR "BareImplSyntax runtime failed\nExit code: ${rc}\nStdout: ${raw_stdout}\nStderr: ${raw_stderr}")
endif ()
