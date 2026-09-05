# Enable only for the native toolchain with repeated measurements. Other hosts/frontends can opt in explicitly.
set(rux_pch_default OFF)
if (WIN32 AND NOT CMAKE_CROSSCOMPILING
        AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$"
        AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU"
        AND NOT CMAKE_CXX_COMPILER_TARGET MATCHES "^(aarch64|arm64)")
    set(rux_pch_default ON)
endif ()
option(RUX_USE_PCH "Precompile standard-library headers and the unit-test framework" ${rux_pch_default})
option(RUX_THIN_LTO "Use ThinLTO for optimized configurations" OFF)
option(RUX_DEAD_STRIP "Place functions/data in separate sections and remove unused optimized code" OFF)

set(RUX_STDLIB_PCH_HEADERS
    <algorithm> <array> <chrono> <cstddef> <cstdint> <filesystem> <format> <functional>
    <map> <memory> <optional> <span> <string> <string_view> <unordered_map> <unordered_set>
    <utility> <vector>)
set_property(GLOBAL PROPERTY RUX_STDLIB_PCH_HEADERS "${RUX_STDLIB_PCH_HEADERS}")

add_library(RuxBuildOptions INTERFACE)
set(rux_optimized "$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>")
if (RUX_DEAD_STRIP)
    target_compile_options(RuxBuildOptions INTERFACE
        "$<$<AND:${rux_optimized},$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,GNU>>:-ffunction-sections;-fdata-sections>"
        "$<$<AND:${rux_optimized},$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,MSVC>>:/Gy;/Gw>")
    if (WIN32)
        # lld-link already defaults to REF/ICF for an optimized link; spell out the required behavior for other linkers.
        target_link_options(RuxBuildOptions INTERFACE "$<${rux_optimized}:LINKER:/OPT:REF,/OPT:ICF>")
    elseif (APPLE)
        target_link_options(RuxBuildOptions INTERFACE "$<${rux_optimized}:LINKER:-dead_strip>")
    else ()
        target_link_options(RuxBuildOptions INTERFACE "$<${rux_optimized}:LINKER:--gc-sections>")
    endif ()
endif ()

if (RUX_THIN_LTO)
    target_compile_options(RuxBuildOptions INTERFACE
        "$<$<AND:${rux_optimized},$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,GNU>>:-flto=thin>"
        "$<$<AND:${rux_optimized},$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,MSVC>>:/clang:-flto=thin>")
    target_link_options(RuxBuildOptions INTERFACE
        "$<$<AND:${rux_optimized},$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,GNU>>:-flto=thin>")
    set(rux_lto_cache "${PROJECT_BINARY_DIR}/ThinLTO")
    if (WIN32)
        # clang-cl invokes the linker directly through CMake, so select lld there as well as for the GNU driver.
        set(CMAKE_LINKER_TYPE LLD)
        set(CMAKE_LINKER_TYPE LLD PARENT_SCOPE)
        target_link_options(RuxBuildOptions INTERFACE
            "$<$<AND:${rux_optimized},$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,GNU>>:-fuse-ld=lld>"
            "$<${rux_optimized}:LINKER:/lldltocache:${rux_lto_cache}>")
    elseif (APPLE)
        target_link_options(RuxBuildOptions INTERFACE "$<${rux_optimized}:LINKER:-cache_path_lto,${rux_lto_cache}>")
    else ()
        target_link_options(RuxBuildOptions INTERFACE
            "$<${rux_optimized}:-fuse-ld=lld>" "$<${rux_optimized}:LINKER:--thinlto-cache-dir=${rux_lto_cache}>")
    endif ()
endif ()

function(rux_configure_precompiled_headers)
    if (NOT RUX_USE_PCH)
        return()
    endif ()
    file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/Compiler/Pch.cpp" CONTENT "// Standard-library PCH anchor.\n")
    add_library(RuxStdPch OBJECT "${PROJECT_BINARY_DIR}/Compiler/Pch.cpp")
    target_link_libraries(RuxStdPch PRIVATE RuxWarnings RuxBuildOptions)
    target_include_directories(RuxStdPch PRIVATE "${PROJECT_SOURCE_DIR}/Compiler" "${PROJECT_BINARY_DIR}/Compiler/generated")
    target_precompile_headers(RuxStdPch PRIVATE ${RUX_STDLIB_PCH_HEADERS})
    get_property(rux_pch_targets GLOBAL PROPERTY RUX_COMPONENT_TARGETS)
    list(REMOVE_ITEM rux_pch_targets RuxBuildInfo)
    foreach (rux_pch_target IN LISTS rux_pch_targets ITEMS rux)
        target_precompile_headers(${rux_pch_target} REUSE_FROM RuxStdPch)
    endforeach ()
    set_property(GLOBAL APPEND PROPERTY RUX_COMPONENT_TARGETS RuxStdPch)
endfunction()
