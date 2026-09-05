// Entry point for the C++ unit tests (see Tests/Unit/CMakeLists.txt).
//
// Every *Tests.cpp registers its TEST_CASEs here. Probe modes exercise real child process streams without shell tools.

#define DOCTEST_CONFIG_IMPLEMENT
#include "ProcessProbe.h"

#include <doctest.h>

int main(int argc, char **argv) {
    if (const auto probe = Rux::Testing::RunProcessProbe(argc, argv))
        return *probe;
    return doctest::Context(argc, argv).run();
}
