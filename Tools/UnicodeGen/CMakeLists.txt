# The Unicode table generator: reads the UCD files under Data/ and writes the generated tables of Packages/Unicode.
# Run by hand; the output is committed, so CI builds the tool but never runs it.
add_executable(rux-unicode-gen Main.cpp)
set_target_properties(rux-unicode-gen PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Bin/Tools)
