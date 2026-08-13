#include "Driver/BuildTarget.h"
#include "System/Os.h"
#include "Target/Platform.h"

#include <array>
#include <doctest.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

TEST_CASE("host architecture information identifies the running compiler process") {
    const auto architectures = GetHostArchitectureInfo();
    CHECK(architectures.processArch == HostArch);
    CHECK(architectures.nativeArch != Arch::Unknown);
    if constexpr (HostOS != OS::Windows && HostOS != OS::MacOS) {
        CHECK(architectures.nativeArch == HostArch);
    }
    if constexpr ((HostOS == OS::Windows || HostOS == OS::MacOS) && HostArch == Arch::X86_64) {
        if (architectures.nativeArch == Arch::AArch64) {
            const std::string_view nativeTarget = HostOS == OS::Windows ? "windows-aarch64" : "macos-aarch64";
            CHECK(Rux::Driver::HostCanExecuteTarget(*TargetTriple::Parse(nativeTarget)));
        }
    }
}

TEST_CASE("ExecutableFileName appends .exe only for Windows targets") {
    CHECK(ExecutableFileName("rux", OS::Windows) == "rux.exe");
    CHECK(ExecutableFileName("rux", OS::Linux) == "rux");
    CHECK(ExecutableFileName("rux", OS::MacOS) == "rux");
}

TEST_CASE("library file names follow target platform conventions") {
    CHECK(SharedLibraryFileName("Std", OS::Windows) == "Std.dll");
    CHECK(SharedLibraryFileName("Std", OS::Linux) == "libStd.so");
    CHECK(SharedLibraryFileName("Std", OS::FreeBSD) == "libStd.so");
    CHECK(SharedLibraryFileName("Std", OS::MacOS) == "libStd.dylib");
    CHECK(StaticLibraryFileName("Std", OS::Windows) == "Std.lib");
    CHECK(StaticLibraryFileName("Std", OS::Linux) == "libStd.a");
    CHECK(StaticLibraryFileName("Std", OS::MacOS) == "libStd.a");
}

TEST_CASE("workspace platform package names match their target triples") {
    using namespace Rux::Driver;

    CHECK(IsPlatformPackageName("Linux"));
    CHECK(IsPlatformPackageName("MacOS"));
    CHECK(IsPlatformPackageName("FreeBSD"));
    CHECK(PlatformPackageMatchesTarget("Linux", *TargetTriple::Parse("linux-x86_64")));
    CHECK(PlatformPackageMatchesTarget("MacOS", *TargetTriple::Parse("macos-aarch64")));
    CHECK(PlatformPackageMatchesTarget("FreeBSD", *TargetTriple::Parse("freebsd-x86_64")));
    CHECK_FALSE(PlatformPackageMatchesTarget("Illumos", *TargetTriple::Parse("linux-x86_64")));
}

TEST_CASE("supported targets include native AArch64 hosts") {
    using namespace Rux::Driver;

    CHECK(IsSupportedTargetTriple("freebsd-aarch64"));
    CHECK(IsSupportedTargetTriple("linux-aarch64"));
    CHECK(IsSupportedTargetTriple("macos-aarch64"));
    CHECK(IsSupportedTargetTriple("windows-aarch64"));
    CHECK_FALSE(IsSupportedTargetTriple("openbsd-aarch64"));
}

TEST_CASE("target triples use canonical architecture names and accept compatibility aliases") {
    using namespace Rux::Driver;

    CHECK(CanonicalTargetTriple("linux-x86_64") == "linux-x86_64");
    CHECK(CanonicalTargetTriple("linux-x64") == "linux-x86_64");
    CHECK(CanonicalTargetTriple("linux-amd64") == "linux-x86_64");
    CHECK(CanonicalTargetTriple("linux-x86-64") == "linux-x86_64");
    CHECK(CanonicalTargetTriple("linux-arm64") == "linux-aarch64");
    CHECK(IsSupportedTargetTriple("windows-x64"));
    CHECK(IsSupportedTargetTriple("windows-amd64"));
    CHECK(IsSupportedTargetTriple("windows-arm64"));
}

TEST_CASE("typed target triples reject unknown components without host fallback") {
    CHECK_FALSE(TargetTriple::Parse("plan9-x86_64"));
    CHECK_FALSE(TargetTriple::Parse("linux-riscv64"));
    CHECK_FALSE(TargetTriple::Parse("linux"));
    CHECK_FALSE(TargetTriple::Parse("linux-x86_64-extra"));
    CHECK_FALSE(TargetTriple::From(OS::Unknown, Arch::X86_64));
    CHECK_FALSE(TargetTriple::From(OS::Linux, Arch::Unknown));
}

TEST_CASE("target catalog has one stable canonical and display spelling per supported machine") {
    constexpr std::array canonical = {
        "freebsd-x86_64", "freebsd-aarch64", "linux-x86_64",   "linux-aarch64",
        "macos-x86_64",   "macos-aarch64",   "windows-x86_64", "windows-aarch64",
    };
    const auto supported = TargetTriple::Supported();
    REQUIRE(supported.size() == canonical.size());
    for (std::size_t i = 0; i < supported.size(); ++i) {
        CHECK(supported[i].CanonicalName() == canonical[i]);
        CHECK(TargetTriple::Parse(canonical[i]) == supported[i]);
    }

    const auto windowsX86 = TargetTriple::Parse("windows-x86-64");
    REQUIRE(windowsX86);
    CHECK(windowsX86->CanonicalName() == "windows-x86_64");
    CHECK(windowsX86->DisplayName() == "Windows x86-64");
    CHECK(SupportedTargetTripleNames() == Rux::Driver::SupportedTargetTriples());
}

TEST_CASE("direct target execution accepts process and native OS architectures only") {
    using Rux::Driver::CanExecuteTargetDirectly;

    constexpr Rux::System::HostArchitectureInfo translatedX64{.processArch = Arch::X86_64, .nativeArch = Arch::AArch64};
    CHECK(CanExecuteTargetDirectly(OS::Windows, translatedX64, OS::Windows, Arch::X86_64));
    CHECK(CanExecuteTargetDirectly(OS::Windows, translatedX64, OS::Windows, Arch::AArch64));
    CHECK(CanExecuteTargetDirectly(OS::MacOS, translatedX64, OS::MacOS, Arch::X86_64));
    CHECK(CanExecuteTargetDirectly(OS::MacOS, translatedX64, OS::MacOS, Arch::AArch64));
    CHECK_FALSE(CanExecuteTargetDirectly(OS::Linux, translatedX64, OS::Windows, Arch::X86_64));

    constexpr Rux::System::HostArchitectureInfo physicalX64{.processArch = Arch::X86_64, .nativeArch = Arch::X86_64};
    CHECK(CanExecuteTargetDirectly(OS::Linux, physicalX64, OS::Linux, Arch::X86_64));
    CHECK_FALSE(CanExecuteTargetDirectly(OS::Linux, physicalX64, OS::Linux, Arch::AArch64));
}
