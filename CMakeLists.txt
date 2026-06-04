cmake_minimum_required(VERSION 3.31)
project(Rux VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Handle C++26 cleanly across all toolchains
if (MSVC)
    # Bypasses CMake feature tracking limits by directly enforcing MSVC's latest flag
    add_compile_options(/std:c++latest)
else()
    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

# Target libc++ specifically for Clang on Linux to support C++26 features like <print>
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT APPLE AND UNIX)
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
endif()

set(RUX_ENABLE_IPO_DEFAULT ON)
if (MINGW)
    set(RUX_ENABLE_IPO_DEFAULT OFF)
endif ()

option(RUX_ENABLE_IPO "Enable interprocedural optimization for release builds when supported" ${RUX_ENABLE_IPO_DEFAULT})
option(RUX_ENABLE_NATIVE_OPTIMIZATIONS "Enable CPU-specific native code generation for this host" OFF)

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
        Source/Cli.cpp
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
)

# GCC/MinGW: link experimental libstdc++ extensions (needed for <print> support in some builds)
if (MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_libraries(rux PRIVATE stdc++exp)
endif()

target_include_directories(rux PRIVATE
        Include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_compile_options(rux PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/EHsc>
)

if (RUX_ENABLE_NATIVE_OPTIMIZATIONS)
    if (MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|X86|x86)$")
        target_compile_options(rux PRIVATE /arch:AVX2)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(rux PRIVATE -march=native)
    endif ()
endif ()

if (WIN32)
    target_link_libraries(rux PRIVATE Psapi Winhttp)
endif ()

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    include(CheckCXXSourceCompiles)

    set(_rux_print_check_source [[
        #include <print>
        int main() {
            std::println("ok");
            return 0;
        }
    ]])

    check_cxx_source_compiles("${_rux_print_check_source}" RUX_STD_PRINT_LINKS_WITHOUT_EXTRAS)
    if (NOT RUX_STD_PRINT_LINKS_WITHOUT_EXTRAS)
        set(_rux_saved_required_libraries "${CMAKE_REQUIRED_LIBRARIES}")
        set(CMAKE_REQUIRED_LIBRARIES stdc++exp)
        check_cxx_source_compiles("${_rux_print_check_source}" RUX_STD_PRINT_LINKS_WITH_STDCXXEXP)
        set(CMAKE_REQUIRED_LIBRARIES "${_rux_saved_required_libraries}")

        if (RUX_STD_PRINT_LINKS_WITH_STDCXXEXP)
            target_link_libraries(rux PRIVATE stdc++exp)
        else ()
            message(FATAL_ERROR "std::print is not linkable with this GNU toolchain")
        endif ()
    endif ()
endif ()

if (RUX_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT RUX_IPO_SUPPORTED OUTPUT RUX_IPO_OUTPUT LANGUAGES CXX)
    if (RUX_IPO_SUPPORTED)
        set_property(TARGET rux PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set_property(TARGET rux PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    else ()
        message(STATUS "IPO/LTO disabled: ${RUX_IPO_OUTPUT}")
    endif ()
endif ()

set_property(TARGET rux
        PROPERTY MSVC_RUNTIME_LIBRARY "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreadedDebugDLL>"
)

include(GNUInstallDirs)
install(TARGETS rux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CTest)
if (BUILD_TESTING)
    add_test(
            NAME rux_smoke
            COMMAND ${CMAKE_COMMAND}
                    -DRUX_EXECUTABLE=$<TARGET_FILE:rux>
                    -DRUX_TEST_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/Tests
                    -DRUX_TEST_WORK_ROOT=${CMAKE_CURRENT_BINARY_DIR}/rux_smoke
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/run_smoke_tests.cmake
    )
endif ()
