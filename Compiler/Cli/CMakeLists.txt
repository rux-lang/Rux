# Sources and dependencies owned by Cli.
add_subdirectory(Testing)

rux_add_component(RuxCliContract
    CliSpec.cpp)

rux_add_component(RuxCliHelp
    CliHelpRenderer.cpp)
target_link_libraries(RuxCliHelp PUBLIC RuxCliContract PRIVATE RuxDiagnostics RuxReporting)

rux_add_component(RuxCliReporting
    BuildReport.cpp
    PublicationProblem.cpp
    Reporter.cpp
    TerminalStyle.cpp)
target_link_libraries(RuxCliReporting PUBLIC RuxReporting RuxDriverModel PRIVATE RuxSystem)

target_sources(rux PRIVATE
    Cli.cpp
    CliHelp.cpp
    CmdAuth.cpp
    CmdBuild.cpp
    CmdCheck.cpp
    CmdDependency.cpp
    CmdInspect.cpp
    CmdInstall.cpp
    CmdLint.cpp
    CmdFmt.cpp
    CmdDoc.cpp
    CmdNew.cpp
    CmdPublish.cpp
    CmdRun.cpp
    CmdTest.cpp
)
