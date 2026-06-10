function(add_rux_test name)
    set(test_dir "${CMAKE_SOURCE_DIR}/Tests/${name}")

    add_test(NAME ${name}
            COMMAND ${CMAKE_COMMAND}
            "-DTEST_DIR=${test_dir}"
            "-DRUX=$<TARGET_FILE:rux>"
            "-DTEST_CONFIG=$<CONFIG>"
            -P "${test_dir}/Test.cmake"
            WORKING_DIRECTORY "${test_dir}"
    )

    set_tests_properties(${name} PROPERTIES
            LABELS "${name}"
    )
endfunction()

# Note: Low-level runtime execution tests are skipped on OpenBSD.
# Rux's backend emits raw assembly thunks that utilize direct kernel syscalls,
# which are blocked by OpenBSD's security model (requires libc.so syscall pinning).
if (NOT CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    add_rux_test(BooleanTypes)
    add_rux_test(ConstExpr)
    if (WIN32)
        add_rux_test(Dll)
    endif ()
    add_rux_test(Echo)
    add_rux_test(Factorial)
    add_rux_test(FloatingPointTypes)
    add_rux_test(IntegerTypes)
    add_rux_test(Io)
    add_rux_test(MutualRecursion)
    add_rux_test(NumericSuffix)
    add_rux_test(Pow)
    add_rux_test(TargetImport)
    add_rux_test(Tuple)
    add_rux_test(Unicode)
else ()
    message(STATUS "==========================================================================================")
    message(STATUS "Note: Low-level runtime execution tests are skipped on OpenBSD.")
    message(STATUS "Rux's backend emits raw assembly thunks that utilize direct kernel syscalls,")
    message(STATUS "which are blocked by OpenBSD's security model (requires libc.so syscall pinning).")
    message(STATUS "==========================================================================================")
endif ()