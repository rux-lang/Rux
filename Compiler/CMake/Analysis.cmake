# clang-tidy cannot consume a PCH produced by a different LLVM build. Configure a separate database using the same
# toolchain and warning settings, without changing the development build or compiling another executable.
set(rux_analysis_options "")
foreach(variable CMAKE_CXX_COMPILER CMAKE_CXX_COMPILER_TARGET CMAKE_TOOLCHAIN_FILE CMAKE_SYSROOT
        CMAKE_OSX_SYSROOT CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_BUILD_TYPE
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO
        CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_EXE_LINKER_FLAGS RUX_BUILD_TESTS RUX_WERROR RUX_BUILD_TIMESTAMP)
    if (DEFINED ${variable} AND NOT "${${variable}}" STREQUAL "")
        string(APPEND rux_analysis_options "set(${variable} [==[${${variable}}]==] CACHE STRING \"Analysis toolchain\" FORCE)\n")
    endif ()
endforeach ()
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/AnalysisOptions.cmake" CONTENT "${rux_analysis_options}")
add_custom_target(rux-analysis-database
    COMMAND "${CMAKE_COMMAND}" -S "${PROJECT_SOURCE_DIR}" -B "${PROJECT_BINARY_DIR}/Analysis"
        -G "${CMAKE_GENERATOR}" -C "${PROJECT_BINARY_DIR}/AnalysisOptions.cmake"
        -DRUX_USE_PCH=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    COMMENT "Configure a PCH-disabled compilation database for clang-tidy"
    VERBATIM)
