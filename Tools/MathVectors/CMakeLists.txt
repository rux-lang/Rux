# The math reference generator: computes correctly-rounded references in fixed-point arithmetic and writes the
# boundary vectors of Tests/Packages/Math/Vectors. Run by hand; the output is committed, so CI builds the tool
# but never runs it.
add_executable(rux-math-vectors Main.cpp)
target_link_libraries(rux-math-vectors PRIVATE RuxNumeric)
set_target_properties(rux-math-vectors PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Bin/Tools)

if (WIN32)
    # RuxNumeric is built against the static runtime, so this must match or the link fails on a mismatch.
    set_property(TARGET rux-math-vectors PROPERTY
        MSVC_RUNTIME_LIBRARY
        "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreaded$<$<CONFIG:Debug>:Debug>DLL>")
endif ()
