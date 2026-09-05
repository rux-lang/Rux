# Test discovery, worker execution and scheduling; presentation stays in CmdTest.
find_package(Threads REQUIRED)

rux_add_component(RuxTestRunner
    TestDiscovery.cpp
    TestExecution.cpp
    TestScheduler.cpp)
target_link_libraries(RuxTestRunner PUBLIC RuxDriver PRIVATE Threads::Threads)
target_link_libraries(rux PRIVATE RuxTestRunner)
