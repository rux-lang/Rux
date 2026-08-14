#pragma once

#include "Driver/Registry.h"
#include "Package/Manifest.h"
#include "Target/TargetTriple.h"

#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Driver {
/// One package constraint supplied to the resolver.
struct PackageRequirement {
    IdentitySegment ns;
    IdentitySegment package;
    VersionRange range;
};

/// One exact package version selected for installation.
struct ResolvedPackage {
    IdentitySegment ns;
    IdentitySegment package;
    SemanticVersion version;
};

/// Collect registry dependencies declared by `manifest`, optionally filtering them for one validated target.
[[nodiscard]] std::vector<PackageRequirement>
CollectPackageRequirements(const Manifest &manifest, std::optional<Target::TargetTriple> target = std::nullopt);

using PackageIndexFetcher = std::function<std::expected<RegistryIndexEntry, RegistryError>(
    std::string_view, const IdentitySegment &, const IdentitySegment &)>;

/**
 * @brief Resolves package requirements through one registry.
 *
 * Index documents are cached by normalized package identity for the lifetime of
 * the service. Resolution returns structured failures and never writes to a
 * terminal; command handlers own all rendering.
 */
class PackageResolver {
public:
    explicit PackageResolver(std::string registryBase, PackageIndexFetcher fetchIndex = FetchPackageIndex);

    [[nodiscard]] const std::string &Base() const noexcept;

    [[nodiscard]] std::expected<std::vector<ResolvedPackage>, ResolutionFailure>
    Resolve(std::span<const PackageRequirement> seeds, Target::TargetTriple target,
            const SemanticVersion &compiler = CompilerVersion());

private:
    [[nodiscard]] std::expected<const RegistryIndexEntry *, ResolutionFailure> Get(const IdentitySegment &ns,
                                                                                   const IdentitySegment &package);

    std::string base;
    PackageIndexFetcher fetchIndex;
    std::map<std::string, RegistryIndexEntry> entries;
};
} // namespace Rux::Driver
