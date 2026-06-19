cmake_minimum_required(VERSION 3.31)
project(Rux VERSION 0.3.1 LANGUAGES CXX)

include(CTest)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Handle C++26 cleanly across all toolchains
if (MSVC)
    # Bypasses CMake feature tracking limits by directly enforcing MSVC's latest flag
    add_compile_options(/std:c++latest)
else ()
    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif ()

# Target libc++ specifically for Clang on Linux to support C++26 features like <print>
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT APPLE AND UNIX)
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
endif ()

string(TIMESTAMP RUX_BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP RUX_BUILD_TIME "%H:%M:%S")

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/Include/Rux/Version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/Rux/Version.h
        @ONLY
)

add_executable(rux
        Source/Main.cpp
        Source/Platform.cpp
        Source/Cli/Cli.cpp
        Source/Cli/Commands/HelpCmd.cpp
        Source/Cli/Commands/Base/BuildCmd.cpp
        Source/Cli/Commands/Base/CheckCmd.cpp
        Source/Cli/Commands/Base/InstallCmd.cpp
        Source/Cli/Commands/Base/PackageCmd.cpp
        Source/Cli/Commands/Base/UtilityCmd.cpp
        Source/Manifest.cpp
        Source/Package.cpp
        Source/Token.cpp
        Source/Lexer.cpp
        Source/SourceLoader.cpp
        Source/Parser.cpp
        Source/Sema.cpp
        Source/Hir.cpp
        Source/Lir.cpp
        Source/Asm.cpp
        Source/Rcu.cpp
        Source/Linker.cpp
        Source/Optimizer.cpp
)

# GCC/MinGW: link experimental libstdc++ extensions (needed for <print> support in some builds)
if (MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_libraries(rux PRIVATE stdc++exp)
endif ()

target_include_directories(rux PRIVATE
        Include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)

if (WIN32)
    target_link_libraries(rux PRIVATE Psapi Winhttp)
endif ()

set_property(TARGET rux
        PROPERTY MSVC_RUNTIME_LIBRARY "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreadedDebugDLL>"
)

include(GNUInstallDirs)
install(TARGETS rux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if (BUILD_TESTING)
    add_subdirectory(Tests)
endif ()