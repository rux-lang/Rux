# C++ unit tests for the compiler internals, built only when RUX_BUILD_TESTS
# is ON. Uses the vendored doctest single header (ThirdParty/doctest.h) and
# registers with CTest, so `ctest --test-dir <build>` runs everything.

set(RUX_TEST_SOURCES
    Main.cpp
    BuildReportTests.cpp
    GoldenDiagnosticsTests.cpp
    LexerTests.cpp
    OsTests.cpp
    ProcessTests.cpp
)

if (TARGET RuxFormatter)
    list(APPEND RUX_TEST_SOURCES FormatterTests.cpp)
endif ()
if (TARGET RuxLinter)
    list(APPEND RUX_TEST_SOURCES LinterTests.cpp)
endif ()

add_executable(rux-tests ${RUX_TEST_SOURCES})

target_link_libraries(rux-tests PRIVATE RuxCore)
if (TARGET RuxFormatter)
    target_link_libraries(rux-tests PRIVATE RuxFormatter)
endif ()
if (TARGET RuxLinter)
    target_link_libraries(rux-tests PRIVATE RuxLinter)
endif ()

# SYSTEM keeps the project's warning flags (and -Werror in CI) from applying
# to the vendored doctest header.
target_include_directories(rux-tests SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty)

# doctest's assertion macros expand __COUNTER__ inside our translation units,
# which -Wpedantic flags as a C2y extension under Clang. The RuxWarnings
# interface flags land after target options, so the suppression is attached to
# the sources, whose options come last on the command line.
set_source_files_properties(${RUX_TEST_SOURCES} PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-c2y-extensions>")

# The golden diagnostics runner reads its cases from the source tree.
target_compile_definitions(rux-tests PRIVATE
    RUX_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/Tests/Golden")

# Match the static-CRT choice made for rux/RuxCore in Compiler/CMakeLists.txt;
# mixing runtimes across the RuxCore boundary breaks the link.
if (MSVC OR (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set_property(TARGET rux-tests PROPERTY
        MSVC_RUNTIME_LIBRARY
        "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreaded$<$<CONFIG:Debug>:Debug>DLL>")
endif ()

add_test(NAME UnitTests COMMAND rux-tests)
