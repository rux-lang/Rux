function(rux_register_error code message)
    set_property(GLOBAL PROPERTY "RUX_ERR_${code}" "${message}")
endfunction()

function(rux_prepare_test test_dir bin_name out_binary)
    execute_process(
            COMMAND "${RUX}" build
            WORKING_DIRECTORY "${test_dir}"
            RESULT_VARIABLE build_rc
            OUTPUT_VARIABLE build_log
            ERROR_VARIABLE build_log
    )

    if (NOT build_rc EQUAL 0)
        message(FATAL_ERROR
                "FAIL: Build phase failed (${test_dir})\n"
                "Exit code: ${build_rc}\n"
                "${build_log}"
        )
    endif ()

    if (NOT DEFINED TEST_CONFIG)
        if (DEFINED CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "")
            set(TEST_CONFIG "${CMAKE_BUILD_TYPE}")
        else ()
            set(TEST_CONFIG "Debug")
        endif ()
    endif ()

    set(${out_binary}
            "${test_dir}/Bin/${TEST_CONFIG}/${bin_name}${CMAKE_EXECUTABLE_SUFFIX}"
            PARENT_SCOPE
    )
endfunction()

function(rux_assert_executable bin_path)
    if (NOT EXISTS "${bin_path}")
        message(FATAL_ERROR "FAIL: Binary not found: ${bin_path}")
    endif ()

    execute_process(
            COMMAND "${bin_path}"
            RESULT_VARIABLE run_rc
            OUTPUT_VARIABLE run_stdout
            ERROR_VARIABLE run_stderr
            TIMEOUT 5
    )

    if (run_rc EQUAL 0)
        cmake_path(GET bin_path FILENAME test_executable)
        message(STATUS "PASS: ${test_executable}")
        return()
    endif ()

    get_property(error_msg GLOBAL PROPERTY "RUX_ERR_${run_rc}")
    if (NOT error_msg)
        set(error_msg "Unmapped runtime error code ${run_rc}")
    endif ()

    set(msg "FAIL: ${error_msg}\nExit code: ${run_rc}")

    if (NOT run_stdout STREQUAL "")
        string(APPEND msg "\nStdout:\n${run_stdout}")
    endif ()

    if (NOT run_stderr STREQUAL "")
        string(APPEND msg "\nStderr:\n${run_stderr}")
    endif ()

    message(FATAL_ERROR "${msg}")
endfunction()

function(rux_normalize new_var input)
    string(REGEX REPLACE "\r?\n|\r" "\n" tmp "${input}")
    set(${new_var} "${tmp}" PARENT_SCOPE)
endfunction()

function(rux_add_io_test)
    set(oneValueArgs NAME BINARY STDIN EXPECT)
    cmake_parse_arguments(RUX_IO "" "${oneValueArgs}" "" ${ARGN})

    if (NOT DEFINED RUX_IO_NAME OR NOT DEFINED RUX_IO_BINARY OR NOT DEFINED RUX_IO_EXPECT)
        message(FATAL_ERROR "rux_add_io_test: Missing required arguments (NAME, BINARY, EXPECT)")
    endif ()

    if (NOT DEFINED RUX_IO_STDIN)
        set(RUX_IO_STDIN "")
    endif ()

    if (NOT EXISTS "${RUX_IO_BINARY}")
        message(FATAL_ERROR "Binary not found: ${RUX_IO_BINARY}")
    endif ()

    string(RANDOM LENGTH 8 rand_suffix)
    set(temp_input_file "${CMAKE_CURRENT_BINARY_DIR}/${RUX_IO_NAME}_${rand_suffix}_stdin.txt")
    file(WRITE "${temp_input_file}" "${RUX_IO_STDIN}")

    execute_process(
            COMMAND "${RUX_IO_BINARY}"
            INPUT_FILE "${temp_input_file}"
            OUTPUT_VARIABLE raw_stdout
            ERROR_VARIABLE raw_stderr
            RESULT_VARIABLE rc
            TIMEOUT 5
    )

    file(REMOVE "${temp_input_file}")

    rux_normalize(run_stdout "${raw_stdout}")
    rux_normalize(expected "${RUX_IO_EXPECT}")

    if (NOT rc EQUAL 0)
        message(FATAL_ERROR
                "TEST FAILED: ${RUX_IO_NAME}\n"
                "Exit code: ${rc}\n"
                "Stderr:\n${raw_stderr}\n"
                "Stdout:\n${raw_stdout}\n"
        )
    endif ()

    if (NOT run_stdout STREQUAL expected)
        message(FATAL_ERROR
                "TEST FAILED: ${RUX_IO_NAME}\n"
                "Output mismatch\n\n"
                "EXPECTED:\n${expected}\n\n"
                "ACTUAL:\n${run_stdout}\n"
        )
    endif ()

    message(STATUS "PASS: ${RUX_IO_NAME}")
endfunction()