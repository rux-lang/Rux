# Sources and dependencies owned by System.
rux_add_component(RuxSystem
    Allocator.cpp
    Host.cpp
    Json.cpp
    Os.cpp
    OutputFile.cpp
    Process.cpp)
target_link_libraries(RuxSystem PUBLIC RuxTarget)

# The replacement operator new and delete must be part of the executable itself: a static library member defining them
# would only be linked when something else pulled it in. Executables that want the compiler allocator add this source.
set(RUX_ALLOCATOR_NEW_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/AllocatorNew.cpp CACHE INTERNAL "")

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
