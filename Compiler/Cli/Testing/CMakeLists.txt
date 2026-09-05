# Test discovery, worker execution and scheduling; presentation stays in CmdTest.
rux_add_component(RuxTestRunner
    TestDiscovery.cpp
    TestExecution.cpp
    TestScheduler.cpp)
target_link_libraries(RuxTestRunner PUBLIC RuxDriver)
target_link_libraries(rux PRIVATE RuxTestRunner)

# Worker threads need the platform thread library only outside Windows; see
# Compiler/System/CMakeLists.txt.
if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(RuxTestRunner PRIVATE Threads::Threads)
endif ()
