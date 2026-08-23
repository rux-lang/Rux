# The wide-float reference generator: renders the shortest round-trip decimal of chosen bit patterns with exact
# big-integer arithmetic, checks every rendering against the compiler's own exact parser, and writes the vectors
# of Tests/Packages/Format/WideFloat. Run by hand; the output is committed, so CI builds the tool but never runs it.
add_executable(rux-float-vectors Main.cpp)
target_link_libraries(rux-float-vectors PRIVATE RuxNumeric)
set_target_properties(rux-float-vectors PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Bin/Tools)

if (WIN32)
    # RuxNumeric is built against the static runtime, so this must match or the link fails on a mismatch.
    set_property(TARGET rux-float-vectors PROPERTY
        MSVC_RUNTIME_LIBRARY
        "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreaded$<$<CONFIG:Debug>:Debug>DLL>")
endif ()
