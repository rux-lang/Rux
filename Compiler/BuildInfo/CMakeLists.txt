# Sources and dependencies owned by BuildInfo.

string(TIMESTAMP RUX_BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP RUX_BUILD_TIME "%H:%M:%S")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/CompilerMetadata.h.in
    ${PROJECT_BINARY_DIR}/Compiler/generated/BuildInfo/CompilerMetadata.h
    @ONLY
)

rux_add_interface_component(RuxBuildInfo)
