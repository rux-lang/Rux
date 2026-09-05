#include "Driver/BuildTarget.h"

#include "System/Host.h"
#include "System/Os.h"
#include "Target/Target.h"

#include <algorithm>

namespace Rux::Driver {
using namespace Target;
using namespace System;

std::string TargetName() {
    return Target::TargetTriple::Host().DisplayName();
}

std::string TargetDisplayName(const Target::TargetTriple target) {
    return target.DisplayName();
}

std::string HostTargetTriple() {
    return std::string(Target::TargetTriple::Host().CanonicalName());
}

bool IsSupportedTargetTriple(const std::string_view target) {
    return Target::TargetTriple::Parse(target).has_value();
}

const std::string &SupportedTargetTriples() {
    return Target::SupportedTargetTripleNames();
}

std::string CanonicalTargetTriple(const std::string_view target) {
    const auto parsed = Target::TargetTriple::Parse(target);
    return parsed ? std::string(parsed->CanonicalName()) : std::string(target);
}

std::string_view TargetOsName(const Target::TargetTriple target) {
    return Target::ToString(target.Os());
}

TargetContext TargetContextForTriple(const Target::TargetTriple target) {
    const Target::OS os = target.Os();
    const Target::Arch arch = target.Architecture();
    const Target::DataModel dataModel = os == Target::OS::Windows ? Target::DataModel::LLP64 : Target::DataModel::LP64;
    const Target::ABIInfo abi = Target::GetABIInfo(os, arch, dataModel);
    const bool native = os == Target::HostOS && arch == Target::HostArch;
    return TargetContext{.os = os,
                         .arch = arch,
                         .data_model = dataModel,
                         .abi = abi.abi,
                         .default_cc = abi.cc,
                         .endianness = native ? Target::HostEndianness : Target::Endian::Little,
                         .object_format = Target::GetObjectFormat(os),
                         .pointer_size = Target::GetPointerSize(arch),
                         .cpu_features = native ? Target::HostCpuFeatures : Target::CpuFeature::None};
}

bool HostCanExecuteTarget(const Target::TargetTriple target) {
    return CanExecuteTargetDirectly(HostOS, GetHostArchitectureInfo(), target.Os(), target.Architecture());
}

bool CanExecuteTargetDirectly(const OS hostOs, const HostArchitectureInfo hostArchitectures, const OS targetOs,
                              const Arch targetArch) noexcept {
    return targetOs == hostOs &&
           (targetArch == hostArchitectures.processArch || targetArch == hostArchitectures.nativeArch);
}

bool IsPlatformPackageName(const std::string_view name) {
    const std::string normalized = NormalizeIdentity(name);
    return normalized == "freebsd" || normalized == "linux" || normalized == "macos" || normalized == "windows";
}

bool PlatformPackageMatchesTarget(const std::string_view name, const Target::TargetTriple target) {
    return NormalizeIdentity(name) == NormalizeIdentity(TargetOsName(target));
}

const std::string &DependencyPackageName(const ManifestDependency &dep) {
    return dep.package.Text();
}

std::filesystem::path ResolveRawOutputRoot(const std::filesystem::path &root, const Manifest &manifest) {
    std::filesystem::path output =
        manifest.build.output.empty() ? std::filesystem::path("Bin") : std::filesystem::path(manifest.build.output);
    if (output.is_relative()) {
        output = root / output;
    }
    return output.lexically_normal();
}

std::filesystem::path TargetOutputPath(const Target::TargetTriple target) {
    return std::filesystem::path(Target::ToString(target.Os())) / Target::ToDisplayString(target.Architecture());
}

std::filesystem::path ResolveArtifactOutputDir(const std::filesystem::path &root, const Manifest &manifest,
                                               const BuildProfile profile, const Target::TargetTriple target) {
    return ResolveRawOutputRoot(root, manifest) / ToString(profile) / TargetOutputPath(target);
}

std::filesystem::path ResolveTestOutputDir(const std::filesystem::path &root, const Manifest &manifest,
                                           const Target::TargetTriple target) {
    auto output = ResolveRawOutputRoot(root, manifest);
    if (target != Target::TargetTriple::Host()) {
        output /= TargetOutputPath(target);
    }
    return output;
}

ArtifactKind PackageArtifactKind(const ManifestPackageType type) {
    switch (type) {
    case ManifestPackageType::SharedLibrary:
        return ArtifactKind::SharedLibrary;
    case ManifestPackageType::StaticLibrary:
        return ArtifactKind::StaticLibrary;
    case ManifestPackageType::Executable:
    case ManifestPackageType::SourceLibrary:
        break;
    }
    return ArtifactKind::Executable;
}

std::string OutputFileName(const std::string_view packageName, const ArtifactKind kind, const OS os) {
    switch (kind) {
    case ArtifactKind::SharedLibrary:
        return SharedLibraryFileName(std::string(packageName), os);
    case ArtifactKind::StaticLibrary:
        return StaticLibraryFileName(std::string(packageName), os);
    case ArtifactKind::Executable:
        break;
    }
    return ExecutableFileName(std::string(packageName), os);
}

} // namespace Rux::Driver
