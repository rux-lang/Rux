#include "Target/TargetTriple.h"

#include <array>
#include <cassert>
#include <format>

namespace Rux::Target {
std::optional<TargetTriple> TargetTriple::Parse(const std::string_view text) noexcept {
    const auto separator = text.find('-');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view osText = text.substr(0, separator);
    const std::string_view archText = text.substr(separator + 1);

    OS parsedOs = OS::Unknown;
    if (osText == "freebsd") {
        parsedOs = OS::FreeBSD;
    }
    else if (osText == "linux") {
        parsedOs = OS::Linux;
    }
    else if (osText == "macos") {
        parsedOs = OS::MacOS;
    }
    else if (osText == "windows") {
        parsedOs = OS::Windows;
    }

    Arch parsedArch = Arch::Unknown;
    if (archText == "aarch64" || archText == "arm64") {
        parsedArch = Arch::AArch64;
    }
    else if (archText == "x86_64" || archText == "x86-64" || archText == "x64" || archText == "amd64") {
        parsedArch = Arch::X86_64;
    }

    return From(parsedOs, parsedArch);
}

std::optional<TargetTriple> TargetTriple::From(const OS os, const Arch arch) noexcept {
    if ((os != OS::FreeBSD && os != OS::Linux && os != OS::MacOS && os != OS::Windows) ||
        (arch != Arch::AArch64 && arch != Arch::X86_64)) {
        return std::nullopt;
    }
    return TargetTriple{os, arch};
}

TargetTriple TargetTriple::Host() noexcept {
    const auto host = From(HostOS, HostArch);
    assert(host.has_value());
    return *host;
}

std::span<const TargetTriple> TargetTriple::Supported() noexcept {
    static constexpr std::array catalog{
        TargetTriple{OS::FreeBSD, Arch::X86_64}, TargetTriple{OS::FreeBSD, Arch::AArch64},
        TargetTriple{OS::Linux, Arch::X86_64},   TargetTriple{OS::Linux, Arch::AArch64},
        TargetTriple{OS::MacOS, Arch::X86_64},   TargetTriple{OS::MacOS, Arch::AArch64},
        TargetTriple{OS::Windows, Arch::X86_64}, TargetTriple{OS::Windows, Arch::AArch64},
    };
    return catalog;
}

std::string_view TargetTriple::CanonicalName() const noexcept {
    if (os == OS::FreeBSD) {
        return arch == Arch::AArch64 ? "freebsd-aarch64" : "freebsd-x86_64";
    }
    if (os == OS::Linux) {
        return arch == Arch::AArch64 ? "linux-aarch64" : "linux-x86_64";
    }
    if (os == OS::MacOS) {
        return arch == Arch::AArch64 ? "macos-aarch64" : "macos-x86_64";
    }
    return arch == Arch::AArch64 ? "windows-aarch64" : "windows-x86_64";
}

std::string TargetTriple::DisplayName() const {
    return std::format("{} {}", ToString(os), ToDisplayString(arch));
}

const std::string &SupportedTargetTripleNames() {
    static const std::string names = [] {
        std::string result;
        for (const TargetTriple target : TargetTriple::Supported()) {
            if (!result.empty()) {
                result += ", ";
            }
            result += target.CanonicalName();
        }
        return result;
    }();
    return names;
}
} // namespace Rux::Target
