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

string(TIMESTAMP RUX_BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP RUX_BUILD_TIME "%H:%M:%S")

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/Include/Rux/Version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/Rux/Version.h
        @ONLY
)

file(GLOB_RECURSE RUX_SOURCES 
    Sources/*.cpp
    Include/Rux/*.h
)

add_executable(rux ${RUX_SOURCES})

# GCC/MinGW: link experimental libstdc++ extensions (needed for <print> support in some builds)
if (MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_libraries(rux PRIVATE stdc++exp)
endif()

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
