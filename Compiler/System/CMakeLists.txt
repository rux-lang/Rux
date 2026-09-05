# Sources and dependencies owned by System.
find_package(Threads REQUIRED)

rux_add_component(RuxSystem
    Host.cpp
    Json.cpp
    Os.cpp
    OutputFile.cpp
    Process.cpp)
target_link_libraries(RuxSystem PUBLIC RuxTarget PRIVATE Threads::Threads)

rux_add_component(RuxHttp
    Http.cpp)
target_link_libraries(RuxHttp PUBLIC RuxSystem)
if (WIN32)
    target_link_libraries(RuxHttp PRIVATE winhttp)
endif ()
