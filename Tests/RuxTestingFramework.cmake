function(rux_register_error code message)
    set_property(GLOBAL PROPERTY "RUX_ERR_${code}" "${message}")
endfunction()

function(rux_prepare_test test_dir bin_name out_binary)
    set(config_dir "Debug")
    set(rux_args "")

    if (TEST_CONFIG STREQUAL "Release")
        set(config_dir "Release")
        set(rux_args "--release")
    endif ()

    execute_process(
            COMMAND "${RUX}" build ${rux_args}
            WORKING_DIRECTORY "${test_dir}"
            RESULT_VARIABLE build_rc
            OUTPUT_VARIABLE build_log
            ERROR_VARIABLE build_log
    )

    if (NOT build_rc EQUAL 0)
        message(FATAL_ERROR "Build failed for ${bin_name}:\n${build_log}")
    endif ()

    if (WIN32)
        set(expected_filename "${bin_name}.exe")
    else ()
        set(expected_filename "${bin_name}")
    endif ()

    cmake_path(APPEND test_dir "Bin" "${config_dir}" "${expected_filename}" OUTPUT_VARIABLE target_bin_path)

    if (NOT EXISTS "${target_bin_path}")
        message(STATUS "DEBUG: Binary not found at ${target_bin_path}")
        message(STATUS "DEBUG: Directory contents of ${test_dir}")

        file(GLOB_RECURSE found_files "${test_dir}/*")
        foreach (f ${found_files})
            message(STATUS "Found: ${f}")
        endforeach ()

        message(FATAL_ERROR "Binary not found at expected path: ${target_bin_path}")
    endif ()

    set(${out_binary} "${target_bin_path}" PARENT_SCOPE)
endfunction()

function(rux_assert_executable bin_path)
    if (NOT EXISTS "${bin_path}")
        message(FATAL_ERROR "Binary not found: ${bin_path}")
    endif ()

    execute_process(
            COMMAND "${bin_path}"
            RESULT_VARIABLE run_rc
            OUTPUT_VARIABLE run_stdout
            ERROR_VARIABLE run_stderr
            TIMEOUT 5
    )

    if (run_rc EQUAL 0)
        cmake_path(GET bin_path FILENAME test_name)
        message(STATUS "PASS: ${test_name}")
        return()
    endif ()

    get_property(error_msg GLOBAL PROPERTY "RUX_ERR_${run_rc}")
    if (NOT error_msg)
        set(error_msg "Unmapped runtime error code ${run_rc}")
    endif ()

    set(msg "FAIL: ${error_msg}\nExit code: ${run_rc}")
    if (run_stdout)
        string(APPEND msg "\nStdout:\n${run_stdout}")
    endif ()
    if (run_stderr)
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

    if (NOT RUX_IO_NAME OR NOT RUX_IO_BINARY OR NOT RUX_IO_EXPECT)
        message(FATAL_ERROR "rux_add_io_test: Missing required arguments.")
    endif ()

    string(RANDOM LENGTH 8 rand)
    set(stdin_file "${CMAKE_CURRENT_BINARY_DIR}/${RUX_IO_NAME}_${rand}.txt")
    file(WRITE "${stdin_file}" "${RUX_IO_STDIN}")

    execute_process(
            COMMAND "${RUX_IO_BINARY}"
            INPUT_FILE "${stdin_file}"
            OUTPUT_VARIABLE raw_stdout
            ERROR_VARIABLE raw_stderr
            RESULT_VARIABLE rc
            TIMEOUT 5
    )

    file(REMOVE "${stdin_file}")

    rux_normalize(actual "${raw_stdout}")
    rux_normalize(expected "${RUX_IO_EXPECT}")

    if (NOT rc EQUAL 0)
        message(FATAL_ERROR "TEST FAILED: ${RUX_IO_NAME}\nExit code: ${rc}\nStderr: ${raw_stderr}")
    endif ()

    if (NOT actual STREQUAL expected)
        message(FATAL_ERROR "TEST FAILED: ${RUX_IO_NAME}\n\nEXPECTED:\n${expected}\n\nACTUAL:\n${actual}\n")
    endif ()

    message(STATUS "PASS: ${RUX_IO_NAME}")
endfunction()