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
    CHECK(IsPlatformPackageName("Bsd"));
    CHECK(PlatformPackageMatchesTarget("Linux", "linux-x86_64"));
    CHECK(PlatformPackageMatchesTarget("MacOS", "macos-aarch64"));
    CHECK(PlatformPackageMatchesTarget("Bsd", "freebsd-x86_64"));
    CHECK_FALSE(PlatformPackageMatchesTarget("Illumos", "linux-x86_64"));
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

namespace {
using namespace Rux::Driver;

// A triple naming this host's operating system and some other architecture,
// which is the only combination a user-mode emulator answers for.
std::string ForeignArchTriple() {
    const auto host = HostTargetTriple();
    const auto os = host.substr(0, host.find('-'));
    return os + (HostArch == Arch::X86_64 ? "-aarch64" : "-x86_64");
}

// A triple naming another operating system, which no emulator answers for.
std::string ForeignOsTriple() {
    return HostOS == OS::Windows ? "linux-x86_64" : "windows-x86_64";
}

// The compiler this build produced, standing in for an emulator: resolution
// only needs a file it can run, not one that emulates anything.
std::filesystem::path RuxExecutable() {
    return std::filesystem::path(RUX_ROOT_DIR) / "Bin" / ExecutableFileName("rux");
}

// Restore RUX_EMULATOR and RUX_QEMU_SYSROOT to unset, whatever a case left.
void ClearEmulatorEnvironment() {
    CHECK(UnsetEnv("RUX_EMULATOR"));
    CHECK(UnsetEnv("RUX_QEMU_SYSROOT"));
}
} // namespace

TEST_CASE("the host's own artifacts run with nothing between them and the host") {
    const auto resolved = ResolveExecutionCommand(HostTargetTriple());
    REQUIRE(resolved.command.has_value());
    CHECK_FALSE(resolved.command->IsEmulated());
    CHECK(resolved.error.empty());
}

TEST_CASE("a foreign operating system is refused rather than emulated") {
    const auto foreign = ForeignOsTriple();
    const auto resolved = ResolveExecutionCommand(foreign);
    CHECK_FALSE(resolved.command.has_value());
    CHECK(resolved.error.contains(foreign));
    CHECK(resolved.error.contains(HostTargetTriple()));
    CHECK(resolved.error.contains("rux build --target"));
}

TEST_CASE("a foreign architecture resolves to its QEMU binary, or names the package holding it") {
    ClearEmulatorEnvironment();
    const auto resolved = ResolveExecutionCommand(ForeignArchTriple());
    // Whether the emulator is installed is a property of the machine running
    // the suite, so both answers are correct — but only one shape of each.
    if (resolved.command) {
        CHECK(resolved.command->IsEmulated());
        CHECK(resolved.command->emulator.filename().string().starts_with("qemu-"));
    }
    else {
        CHECK(resolved.error.contains("qemu-"));
        CHECK(resolved.error.contains("qemu-user"));
    }
}

TEST_CASE("RUX_EMULATOR names the emulator, and its extra words are the emulator's own arguments") {
    ClearEmulatorEnvironment();
    const auto foreign = ForeignArchTriple();

    // A value naming an existing file is one program however it is spelled, so
    // a path holding spaces is not split into pieces.
    REQUIRE(SetEnv("RUX_EMULATOR", RuxExecutable().string()));
    const auto resolved = ResolveExecutionCommand(foreign);
    REQUIRE(resolved.command.has_value());
    CHECK(resolved.command->emulator == RuxExecutable());
    CHECK(resolved.command->emulatorArgs.empty());

    // Anything else is a command line: the first word is the program, and only
    // that word is the one that could not be found.
    REQUIRE(SetEnv("RUX_EMULATOR", "rux-emulator-that-is-not-installed -cpu max"));
    const auto missing = ResolveExecutionCommand(foreign);
    CHECK_FALSE(missing.command.has_value());
    CHECK(missing.error.contains("'rux-emulator-that-is-not-installed'"));
    CHECK_FALSE(missing.error.contains("-cpu"));
    CHECK(missing.error.contains("RUX_EMULATOR"));

    ClearEmulatorEnvironment();
}

TEST_CASE("RUX_QEMU_SYSROOT becomes the emulator's -L, and a directory that is not there is an error") {
    ClearEmulatorEnvironment();
    const auto foreign = ForeignArchTriple();
    REQUIRE(SetEnv("RUX_EMULATOR", RuxExecutable().string()));

    const std::filesystem::path sysroot{RUX_ROOT_DIR};
    REQUIRE(SetEnvPath("RUX_QEMU_SYSROOT", sysroot));
    const auto resolved = ResolveExecutionCommand(foreign);
    REQUIRE(resolved.command.has_value());
    CHECK(resolved.command->emulatorArgs == std::vector<std::string>{"-L", sysroot.string()});

    const auto absent = sysroot / "NoSuchSysrootDirectory";
    REQUIRE(SetEnvPath("RUX_QEMU_SYSROOT", absent));
    const auto refused = ResolveExecutionCommand(foreign);
    CHECK_FALSE(refused.command.has_value());
    CHECK(refused.error.contains("RUX_QEMU_SYSROOT"));
    CHECK(refused.error.contains(absent.string()));

    ClearEmulatorEnvironment();
}

TEST_CASE("an emulated launch puts the artifact after the emulator's arguments and before the program's") {
    const ExecutionCommand emulated{.emulator = "qemu-aarch64", .emulatorArgs = {"-L", "/sysroot"}};
    const std::array<std::string_view, 2> args{"--verbose", "input.txt"};

    const auto launch = PrepareLaunch(emulated, "Bin/App", args);
    CHECK(launch.program == std::filesystem::path("qemu-aarch64"));
    CHECK(launch.args == std::vector<std::string>{"-L", "/sysroot", "Bin/App", "--verbose", "input.txt"});
    CHECK(launch.CommandLine() == "qemu-aarch64 -L /sysroot Bin/App --verbose input.txt");

    // Without an emulator the artifact is the program, and its arguments are
    // the only ones there are.
    const auto direct = PrepareLaunch(ExecutionCommand{}, "Bin/App", args);
    CHECK(direct.program == std::filesystem::path("Bin/App"));
    CHECK(direct.args == std::vector<std::string>{"--verbose", "input.txt"});
}
