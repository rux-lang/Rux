cmake_minimum_required(VERSION 4.2)
project(Rux VERSION 0.1.2)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

string(TIMESTAMP RUX_BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP RUX_BUILD_TIME "%H:%M:%S")

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/Include/Rux/Version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/Rux/Version.h
        @ONLY
)

add_executable(rux
        Source/Main.cpp
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
        Include/Rux/Cli.h
        Include/Rux/Manifest.h
        Include/Rux/Package.h
        Include/Rux/Token.h
        Include/Rux/Lexer.h
        Include/Rux/SourceLoader.h
        Include/Rux/Ast.h
        Include/Rux/Parser.h
        Include/Rux/Type.h
        Include/Rux/Sema.h
        Include/Rux/Hir.h
        Include/Rux/Lir.h
        Include/Rux/Asm.h
        Include/Rux/Rcu.h
        Include/Rux/Linker.h
)

target_include_directories(rux PRIVATE
        Include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)

set_property(TARGET rux
        PROPERTY MSVC_RUNTIME_LIBRARY "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreadedDebugDLL>"
)
