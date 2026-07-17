#include "Driver/BuildTarget.h"
#include "System/Os.h"
#include "Target/Platform.h"

#include <doctest.h>

using namespace Rux::System;
using namespace Rux::Target;

TEST_CASE("GetEnv returns nullopt for unset variables") {
    constexpr const char *unset = "RUX_TEST_VARIABLE_THAT_IS_NEVER_SET";
    CHECK(!GetEnv(unset).has_value());
    CHECK(!GetEnvPath(unset).has_value());
    CHECK(!HasEnv(unset));
}

TEST_CASE("GetEnv reads variables that exist") {
    // PATH is set in every environment the compiler builds in.
    REQUIRE(HasEnv("PATH"));
    const auto value = GetEnv("PATH");
    REQUIRE(value.has_value());
    CHECK(!value->empty());
    CHECK(GetEnvPath("PATH").has_value());
}

TEST_CASE("TempDirectory points at an existing directory") {
    const auto dir = TempDirectory();
    REQUIRE(!dir.empty());
    CHECK(std::filesystem::is_directory(dir));
}

TEST_CASE("WindowsSystemDirectory is populated only on Windows hosts") {
    const auto dir = WindowsSystemDirectory();
    if constexpr (HostOS == OS::Windows) {
        CHECK(!dir.empty());
    }
    else {
        CHECK(dir.empty());
    }
}

TEST_CASE("PeakMemoryBytes reports a nonzero value on supported hosts") {
#if RUX_OS_WINDOWS || RUX_IS_UNIX
    CHECK(PeakMemoryBytes() > 0);
#else
    CHECK(PeakMemoryBytes() == 0);
#endif
}

TEST_CASE("ExecutableFileName appends .exe only for Windows targets") {
    CHECK(ExecutableFileName("rux", OS::Windows) == "rux.exe");
    CHECK(ExecutableFileName("rux", OS::Linux) == "rux");
    CHECK(ExecutableFileName("rux", OS::MacOS) == "rux");
}

TEST_CASE("SharedLibraryFileName appends .dll only for Windows targets") {
    CHECK(SharedLibraryFileName("Std", OS::Windows) == "Std.dll");
    CHECK(SharedLibraryFileName("Std", OS::Linux) == "Std");
    CHECK(SharedLibraryFileName("Std", OS::FreeBSD) == "Std");
}

TEST_CASE("workspace platform package names match their target triples") {
    using namespace Rux::Driver;

    CHECK(IsPlatformPackageName("Linux"));
    CHECK(IsPlatformPackageName("MacOS"));
    CHECK(IsPlatformPackageName("Bsd"));
    CHECK(PlatformPackageMatchesTarget("Linux", "linux-x64"));
    CHECK(PlatformPackageMatchesTarget("MacOS", "macos-aarch64"));
    CHECK(PlatformPackageMatchesTarget("Bsd", "freebsd-x64"));
    CHECK_FALSE(PlatformPackageMatchesTarget("Illumos", "linux-x64"));
}
