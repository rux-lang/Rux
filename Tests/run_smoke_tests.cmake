if (NOT DEFINED RUX_EXECUTABLE OR RUX_EXECUTABLE STREQUAL "")
    message(FATAL_ERROR "RUX_EXECUTABLE is required")
endif ()

if (NOT DEFINED RUX_TEST_ROOT OR RUX_TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "RUX_TEST_ROOT is required")
endif ()

if (NOT DEFINED RUX_TEST_WORK_ROOT OR RUX_TEST_WORK_ROOT STREQUAL "")
    set(RUX_TEST_WORK_ROOT "${CMAKE_BINARY_DIR}/rux_smoke")
endif ()

function(rux_fail MESSAGE_TEXT)
    message(FATAL_ERROR "${MESSAGE_TEXT}")
endfunction()

function(rux_prepare_package OUT_VAR PACKAGE_NAME)
    set(src_dir "${RUX_TEST_ROOT}/${PACKAGE_NAME}")
    set(work_dir "${RUX_TEST_WORK_ROOT}/${PACKAGE_NAME}")
    file(REMOVE_RECURSE "${work_dir}")
    file(MAKE_DIRECTORY "${RUX_TEST_WORK_ROOT}")
    file(COPY "${src_dir}" DESTINATION "${RUX_TEST_WORK_ROOT}")
    set("${OUT_VAR}" "${work_dir}" PARENT_SCOPE)
endfunction()

function(rux_build_package PACKAGE_DIR)
    execute_process(
            COMMAND "${RUX_EXECUTABLE}" build
            WORKING_DIRECTORY "${PACKAGE_DIR}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error
    )
    if (NOT result EQUAL 0)
        rux_fail("rux build failed in ${PACKAGE_DIR}\n${output}${error}")
    endif ()
endfunction()

function(rux_find_artifact OUT_VAR PACKAGE_DIR PROFILE NAME)
    set(base "${PACKAGE_DIR}/Bin/${PROFILE}/${NAME}")
    set(candidates "${base}" "${base}.exe" "${base}.dll" "${base}.dylib" "${base}.so")
    foreach (candidate IN LISTS candidates)
        if (EXISTS "${candidate}")
            set("${OUT_VAR}" "${candidate}" PARENT_SCOPE)
            return()
        endif ()
    endforeach ()
    rux_fail("expected build artifact was not found for ${NAME} in ${PACKAGE_DIR}/Bin/${PROFILE}")
endfunction()

function(rux_run_exit_zero PACKAGE_NAME BINARY_NAME)
    rux_prepare_package(package_dir "${PACKAGE_NAME}")
    rux_build_package("${package_dir}")
    rux_find_artifact(binary "${package_dir}" "Debug" "${BINARY_NAME}")
    execute_process(
            COMMAND "${binary}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error
    )
    if (NOT result EQUAL 0)
        rux_fail("${PACKAGE_NAME} exited with ${result}\n${output}${error}")
    endif ()
    message(STATUS "PASS: ${PACKAGE_NAME}")
endfunction()

function(rux_run_expect_output PACKAGE_NAME BINARY_NAME EXPECTED)
    rux_prepare_package(package_dir "${PACKAGE_NAME}")
    rux_build_package("${package_dir}")
    rux_find_artifact(binary "${package_dir}" "Debug" "${BINARY_NAME}")
    execute_process(
            COMMAND "${binary}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error
    )
    string(STRIP "${output}" actual)
    if ((NOT result EQUAL 0) OR (NOT actual STREQUAL EXPECTED))
        rux_fail("${PACKAGE_NAME} expected [${EXPECTED}], got [${actual}] with exit ${result}\n${error}")
    endif ()
    message(STATUS "PASS: ${PACKAGE_NAME}")
endfunction()

function(rux_run_expect_stdin PACKAGE_NAME BINARY_NAME INPUT EXPECTED_SUBSTRING)
    rux_prepare_package(package_dir "${PACKAGE_NAME}")
    rux_build_package("${package_dir}")
    rux_find_artifact(binary "${package_dir}" "Debug" "${BINARY_NAME}")
    set(stdin_file "${RUX_TEST_WORK_ROOT}/rux_test_stdin.txt")
    file(WRITE "${stdin_file}" "${INPUT}")
    execute_process(
            COMMAND "${binary}"
            INPUT_FILE "${stdin_file}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error
    )
    string(FIND "${output}" "${EXPECTED_SUBSTRING}" substring_index)
    if ((NOT result EQUAL 0) OR (substring_index EQUAL -1))
        rux_fail("${PACKAGE_NAME} expected output containing [${EXPECTED_SUBSTRING}], got [${output}] with exit ${result}\n${error}")
    endif ()
    message(STATUS "PASS: ${PACKAGE_NAME}")
endfunction()

function(rux_run_build_only PACKAGE_NAME)
    rux_prepare_package(package_dir "${PACKAGE_NAME}")
    rux_build_package("${package_dir}")
    message(STATUS "PASS: ${PACKAGE_NAME}")
endfunction()

if (CMAKE_HOST_SYSTEM_NAME STREQUAL "OpenBSD")
    message(STATUS "SKIP: runtime smoke tests are disabled on OpenBSD")
    file(REMOVE_RECURSE "${RUX_TEST_WORK_ROOT}")
    return()
endif ()

rux_run_exit_zero("Pow" "pow_test")
rux_run_exit_zero("Unicode" "unicode_test")
rux_run_exit_zero("NumericSuffix" "numeric_suffix_test")
rux_run_exit_zero("Tuple" "tuple_test")
rux_run_build_only("TargetImport")
rux_run_expect_output("Io" "io_test" "Hello from a Rux binary via I/O thunks!")
rux_run_expect_stdin("Echo" "echo_test" "dummy input" "read stdin OK")

if (CMAKE_HOST_WIN32)
    rux_prepare_package(dll_package "Dll")
    rux_build_package("${dll_package}")
    rux_find_artifact(dll "${dll_package}" "Debug" "dll_test")
    message(STATUS "PASS: Dll")
else ()
    message(STATUS "SKIP: Dll is a Windows PE artifact test")
endif ()

file(REMOVE_RECURSE "${RUX_TEST_WORK_ROOT}")
