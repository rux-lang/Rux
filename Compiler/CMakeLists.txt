option(RUX_WERROR "Treat compiler warnings as errors" OFF)

# Warning flags shared by every Rux target. Interface-only, so consumers pick
# the flags up via target_link_libraries. The frontend variant (not the
# compiler id) selects the flag style. CMake calls these frontend variants
# MSVC for clang-cl and GNU for the ordinary clang++ driver.
add_library(RuxWarnings INTERFACE)
target_compile_options(RuxWarnings INTERFACE
    $<$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,MSVC>:/W4 /permissive- /utf-8>
    $<$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,GNU>:-Wall -Wextra -Wpedantic -Wshadow>)
if (RUX_WERROR)
    target_compile_options(RuxWarnings INTERFACE
        $<IF:$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,MSVC>,/WX,-Werror>)
endif ()

function(rux_add_component target)
    add_library(${target} STATIC ${ARGN})
    set_property(GLOBAL APPEND PROPERTY RUX_COMPONENT_TARGETS ${target})
    target_include_directories(${target} PUBLIC
        ${PROJECT_SOURCE_DIR}/Compiler
        ${PROJECT_BINARY_DIR}/Compiler/generated)
    target_link_libraries(${target} PUBLIC RuxWarnings)
endfunction()

function(rux_add_interface_component target)
    add_library(${target} INTERFACE)
    target_include_directories(${target} INTERFACE
        ${PROJECT_SOURCE_DIR}/Compiler
        ${PROJECT_BINARY_DIR}/Compiler/generated)
    target_link_libraries(${target} INTERFACE RuxWarnings)
endfunction()

add_executable(rux Main.cpp)

add_subdirectory(SourceModel)
add_subdirectory(BuildInfo)
add_subdirectory(Numeric)
add_subdirectory(Unicode)
add_subdirectory(Reporting)
add_subdirectory(Target)
add_subdirectory(Diagnostics)
add_subdirectory(Cli)
add_subdirectory(Source)
add_subdirectory(System)
add_subdirectory(Crypto)
add_subdirectory(Package)
add_subdirectory(Types)
add_subdirectory(Lexer)
add_subdirectory(Syntax)
add_subdirectory(Semantic/Model)
add_subdirectory(Semantic)
add_subdirectory(Formatter)
add_subdirectory(Documentation)
add_subdirectory(Linter)
add_subdirectory(Ir/Hir)
add_subdirectory(Ir/Lir)
add_subdirectory(Optimization)
add_subdirectory(Lowering)
add_subdirectory(Object)
add_subdirectory(CodeGen)
add_subdirectory(CodeGen/X86_64)
add_subdirectory(Linker)
add_subdirectory(CodeGen/AArch64)
add_subdirectory(Driver)

# Compatibility aggregation target for embedders and existing tests. It owns
# no sources; dependency validation happens in the component targets above.
add_library(RuxCore INTERFACE)
target_link_libraries(RuxCore INTERFACE RuxDriver RuxPackageServices)

target_link_libraries(rux PRIVATE
    RuxDriver RuxPackageServices RuxFormatter RuxLinter RuxDocumentation RuxCliContract RuxCliHelp RuxCliReporting RuxReporting)

# Emit the compiler directly to Bin/ in the source tree while keeping CMake
# intermediates in the build directory. Configuration-specific properties keep
# multi-config generators from appending Debug/ or Release/ subdirectories.
set_target_properties(rux PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/Bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/Bin
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/Bin
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_SOURCE_DIR}/Bin)

if (WIN32)
    get_property(rux_components GLOBAL PROPERTY RUX_COMPONENT_TARGETS)
    set_property(TARGET rux ${rux_components} PROPERTY
        MSVC_RUNTIME_LIBRARY
        "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreaded$<$<CONFIG:Debug>:Debug>DLL>")
endif ()

include(GNUInstallDirs)
install(TARGETS rux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
