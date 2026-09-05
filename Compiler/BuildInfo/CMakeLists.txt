# Sources and dependencies owned by BuildInfo.

# Invalidate dependencies on the former generated public header when upgrading an existing build tree. Otherwise
# Ninja can retain objects that still define inline metadata beside the new implementation-unit definitions.
file(REMOVE "${PROJECT_BINARY_DIR}/Compiler/generated/BuildInfo/CompilerMetadata.h")

set(RUX_BUILD_TIMESTAMP "" CACHE STRING "Override compiler metadata timestamp (YYYY-MM-DDTHH:MM:SSZ, UTC)")
if (RUX_BUILD_TIMESTAMP)
    set(rux_metadata_timestamp "${RUX_BUILD_TIMESTAMP}")
elseif (DEFINED ENV{SOURCE_DATE_EPOCH})
    # CMake validates and honors SOURCE_DATE_EPOCH when formatting timestamps.
    string(TIMESTAMP rux_metadata_timestamp "%Y-%m-%dT%H:%M:%SZ" UTC)
else ()
    if (NOT DEFINED RUX_INITIAL_BUILD_TIMESTAMP)
        string(TIMESTAMP rux_initial_timestamp "%Y-%m-%dT%H:%M:%SZ" UTC)
        set(RUX_INITIAL_BUILD_TIMESTAMP "${rux_initial_timestamp}" CACHE INTERNAL "Initial build-tree timestamp")
    endif ()
    set(rux_metadata_timestamp "${RUX_INITIAL_BUILD_TIMESTAMP}")
endif ()
if (NOT rux_metadata_timestamp MATCHES "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z$")
    message(FATAL_ERROR "RUX_BUILD_TIMESTAMP must use YYYY-MM-DDTHH:MM:SSZ (UTC)")
endif ()
string(SUBSTRING "${rux_metadata_timestamp}" 0 10 RUX_BUILD_DATE)
string(SUBSTRING "${rux_metadata_timestamp}" 11 8 RUX_BUILD_TIME)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/CompilerMetadata.cpp.in
    ${PROJECT_BINARY_DIR}/Compiler/generated/BuildInfo/CompilerMetadata.cpp
    @ONLY
)

rux_add_component(RuxBuildInfo ${PROJECT_BINARY_DIR}/Compiler/generated/BuildInfo/CompilerMetadata.cpp)
