include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

# rux_prepare_test runs `rux build`. If collectImports() triggers an error
# due to the unlisted platform dependencies on the current host,
# this function intercepts the non-zero status code and fails the test instantly.
rux_prepare_test("${TEST_DIR}" "target_import_test" target_binary)

message(STATUS "PASS: ${TEST_NAME}")