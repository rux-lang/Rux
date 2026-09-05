# Every translation unit belongs to one group. The runtime inventory test also checks actual doctest registrations,
# including skipped cases, so a renamed file or a wildcard overlap cannot silently lose or duplicate coverage.
set(rux_unit_groups Numeric Frontend Lowering Backends Driver Cli Optimization)
foreach (source IN LISTS RUX_TEST_SOURCES)
    if (source MATCHES "^(SoftwareFloat|WideInteger|FloatFormat|AllocationMath|PrimitiveCatalog|PrimitiveConstants|Utf|Checksum|Json|AArch64Encoder)Tests\\.cpp$")
        set(group Numeric)
    elseif (source MATCHES "^(Cli|Cmd|UserMessage|InspectionOutput|Reporter|Reporting|BuildReport)")
        set(group Cli)
    elseif (source MATCHES "^(CompilerDriver|CompilerPipeline|BuildPlan|Package|Manifest|Credentials|Registry|Process|Os|OutputFile|SourceLoader|Artifact)")
        set(group Driver)
    elseif (source MATCHES "^(Optimizer|LirOptimization|LirReachability)")
        set(group Optimization)
    elseif (source MATCHES "(Lowering|DropGlue|Consumption|Lifecycle|Cleanup)")
        set(group Lowering)
    elseif (source MATCHES "^(AArch64|X86_64|Codegen|StringCodeGen|Linker|PeLinker|MachO|Rcu|FreeBSD|Backend|PlatformAsm)")
        set(group Backends)
    else ()
        set(group Frontend)
    endif ()
    list(APPEND rux_unit_sources_${group} "*/${source}")
endforeach ()

# Reference vectors register cases from an included file (MSVC-compatible preprocessors use a backslash here).
list(APPEND rux_unit_sources_Numeric "*AArch64EncoderVectors.inc")

set(RUX_UNIT_FILTERS)
foreach (group IN LISTS rux_unit_groups)
    list(JOIN rux_unit_sources_${group} "," filter)
    list(APPEND RUX_UNIT_FILTERS "${filter}")
    add_test(NAME Unit.${group} COMMAND rux-tests "--source-file=${filter}")
    set_tests_properties(Unit.${group} PROPERTIES LABELS Unit)
    # These groups include fixture compilation or process/global-environment setup. Preserve their existing artifact
    # paths with one resource lock; pure numeric/reference-vector tests can run alongside them.
    if (NOT group STREQUAL Numeric)
        set_tests_properties(Unit.${group} PROPERTIES RESOURCE_LOCK RuxUnitArtifacts)
    endif ()
endforeach ()

configure_file(VerifyGroups.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/VerifyGroups.cmake" @ONLY)
add_test(NAME Unit.GroupCoverage COMMAND "${CMAKE_COMMAND}" "-DRUX_TEST_EXECUTABLE=$<TARGET_FILE:rux-tests>"
    -P "${CMAKE_CURRENT_BINARY_DIR}/VerifyGroups.cmake")
set_tests_properties(Unit.GroupCoverage PROPERTIES LABELS Unit)
