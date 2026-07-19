cmake_minimum_required(VERSION 4.3)
project(Rux VERSION 0.3.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif ()

option(RUX_BUILD_TESTS "Build the C++ unit tests (Tests/Unit)" OFF)

# The compiler is a self-contained component; its target lives in Compiler/.
add_subdirectory(Compiler)

if (RUX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests/Unit)
endif ()
