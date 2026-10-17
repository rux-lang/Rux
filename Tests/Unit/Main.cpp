// Entry point for the C++ unit tests (see Tests/Unit/CMakeLists.txt).
//
// Every *Tests.cpp registers its TEST_CASEs here. Probe modes exercise real child process streams without shell tools.

#define DOCTEST_CONFIG_IMPLEMENT
#include "ProcessProbe.h"
#include "System/Os.h"

#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <string>
#include <system_error>

int main(int argc, char **argv) {
    if (const auto probe = Rux::Testing::RunProcessProbe(argc, argv))
        return *probe;
    // Compiling keeps dependency packages in the shared package cache; tests keep theirs, and the compilers they start
    // keep theirs, in a directory of their own instead of the user's, removed when they are done.
    std::filesystem::path packageCache;
    if (!Rux::System::GetEnv("RUX_PACKAGE_CACHE")) {
        packageCache =
            Rux::System::TempDirectory() /
            ("rux-unit-package-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        static_cast<void>(Rux::System::SetEnvPath("RUX_PACKAGE_CACHE", packageCache));
    }
    const int status = doctest::Context(argc, argv).run();
    if (!packageCache.empty()) {
        std::error_code error;
        std::filesystem::remove_all(packageCache, error);
    }
    return status;
}
