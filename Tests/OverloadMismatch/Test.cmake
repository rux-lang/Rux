include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

# Regression #107: Overload resolution must hard-fail on invalid types.
# If this succeeds, Sema is silently picking a bogus overload and trashing the ABI.
set(rux_args "")
if (TEST_CONFIG STREQUAL "Release")
    set(rux_args "--release")
endif ()

execute_process(
    COMMAND "${RUX}" build ${rux_args}
    WORKING_DIRECTORY "${TEST_DIR}"
    RESULT_VARIABLE build_rc
    OUTPUT_VARIABLE build_log
    ERROR_VARIABLE build_log
)

if (build_rc EQUAL 0)
    message(FATAL_ERROR
        "FAIL: overload_mismatch_test compiled successfully — silent fallback is still present.\n"
        "Expected a compile error: no matching overload for 'Sink' with argument types (Foo)")
endif ()

if (NOT build_log MATCHES "no matching overload")
    message(FATAL_ERROR
        "FAIL: Build failed but did not emit the expected 'no matching overload' diagnostic.\n"
        "Compiler output:\n${build_log}")
endif ()

message(STATUS "PASS: overload_mismatch_test correctly rejected with 'no matching overload' error")