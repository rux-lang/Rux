include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

set(test_dir "${TEST_DIR}")

execute_process(
        COMMAND "${RUX}" build
        WORKING_DIRECTORY "${test_dir}"
        RESULT_VARIABLE build_rc
)

if(NOT build_rc EQUAL 0)
    message(FATAL_ERROR "Dll build failed (rc=${build_rc})")
endif()

# --- Expected artifact ---
set(dll "${TEST_DIR}/Bin/Debug/dll_test.dll")

if(NOT EXISTS "${dll}")
    message(FATAL_ERROR "Dll artifact missing: ${dll}")
endif()

message(STATUS "DLL OK: ${dll}")