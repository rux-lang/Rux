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

add_rux_test(BoolBitwise)
add_rux_test(ConstExpr)
if (WIN32)
    add_rux_test(Dll)
endif ()
add_rux_test(Echo)
add_rux_test(Io)
add_rux_test(NumericSuffix)
add_rux_test(Pow)
add_rux_test(TargetImport)
add_rux_test(Tuple)
add_rux_test(Unicode)