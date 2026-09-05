# Sources and dependencies owned by System.
rux_add_component(RuxSystem
    Host.cpp
    Json.cpp
    Os.cpp
    OutputFile.cpp
    Process.cpp)
target_link_libraries(RuxSystem PUBLIC RuxTarget)

# Standard threads need the platform thread library only outside Windows
# (libthr on FreeBSD, libpthread on older glibc). Windows threads are part of
# the CRT, and probing for pthreads there only reports failed checks.
if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(RuxSystem PRIVATE Threads::Threads)
endif ()

rux_add_component(RuxHttp
    Http.cpp)
target_link_libraries(RuxHttp PUBLIC RuxSystem)
if (WIN32)
    target_link_libraries(RuxHttp PRIVATE winhttp)
endif ()
