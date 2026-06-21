message(STATUS "Using rux: ${RUX}")

set(rux_args "")
if (TEST_CONFIG STREQUAL "Release")
    set(rux_args "--release")
endif ()

set(cases
    IfCondition
    ElseIfCondition
    WhileCondition
    DoWhileCondition
    TernaryCondition
    GuardedPattern
    ReturnValue
    LetInitializer
    CallArgument
    NestedGrouping
)

foreach (case_name IN LISTS cases)
    set(case_dir "${TEST_DIR}/${case_name}")
    execute_process(
        COMMAND "${RUX}" build ${rux_args}
        WORKING_DIRECTORY "${case_dir}"
        RESULT_VARIABLE build_rc
        OUTPUT_VARIABLE build_log
        ERROR_VARIABLE build_log
    )

    if (build_rc EQUAL 0)
        message(FATAL_ERROR
            "FAIL: ${case_name} compiled successfully.\n"
            "Assignment expressions must be rejected outside top-level expression statements.")
    endif ()

    if (NOT build_log MATCHES "assignment is not allowed")
        message(FATAL_ERROR
            "FAIL: ${case_name} failed without the expected assignment-expression diagnostic.\n"
            "Compiler output:\n${build_log}")
    endif ()

    message(STATUS "PASS: ${case_name}")
endforeach ()

