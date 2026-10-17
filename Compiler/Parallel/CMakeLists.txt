# Sources and dependencies owned by Parallel.

rux_add_component(RuxParallel
    WorkerPool.cpp)
target_link_libraries(RuxParallel PRIVATE RuxSystem)

# Worker threads need the platform thread library only outside Windows; see
# Compiler/System/CMakeLists.txt.
if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(RuxParallel PUBLIC Threads::Threads)
endif ()
