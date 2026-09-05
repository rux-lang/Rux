find_program(RUX_TEST_SHELL NAMES sh bash HINTS "C:/Program Files/Git/bin" "C:/Program Files/Git/usr/bin")
if (NOT RUX_TEST_SHELL)
    message(FATAL_ERROR "Repository and installer tests require a POSIX shell (Git for Windows supplies one)")
endif ()
if (WIN32)
    set(rux_test_shell_flags --login)
endif ()
# Every section of the repository check is its own test, so CTest runs them
# concurrently; the script defines the section names and each run owns a
# private fixture directory.
foreach (section Workers Help BuildFailures FormatCheck FormatFix Durations Parity)
    add_test(NAME Scripts.Repository.${section} COMMAND "${RUX_TEST_SHELL}" ${rux_test_shell_flags}
        "${CMAKE_CURRENT_SOURCE_DIR}/Repository/Check.sh" ${section})
    set_tests_properties(Scripts.Repository.${section} PROPERTIES LABELS Scripts)
endforeach ()
add_test(NAME Scripts.Installer COMMAND "${RUX_TEST_SHELL}" ${rux_test_shell_flags} "${CMAKE_CURRENT_SOURCE_DIR}/Installer/Check.sh")
set_tests_properties(Scripts.Installer PROPERTIES LABELS Scripts)
if (WIN32)
    # The Git for Windows shell emulates fork, and that emulation hangs under
    # concurrent process creation: a subshell was observed stuck in fork for
    # minutes while the other shell tests spawned their stubs. Run the shell
    # tests one at a time here; they still overlap the C++ groups.
    get_property(rux_script_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${rux_script_tests} PROPERTIES RESOURCE_LOCK MsysFork)
endif ()
