#include "Driver/PackageResolution.h"

#include <unordered_set>
#include <utility>

namespace Rux::Driver {
namespace {
std::string IdentityKey(const IdentitySegment &ns, const IdentitySegment &package) {
    return ns.Normalized() + "/" + package.Normalized();
}

struct Selection {
    IdentitySegment ns;
    IdentitySegment package;
    std::vector<VersionRange> ranges;
    SemanticVersion version;
};
} // namespace

std::vector<PackageRequirement> CollectPackageRequirements(const Manifest &manifest,
                                                           const std::optional<Target::TargetTriple> target) {
    std::vector<PackageRequirement> requirements;
    for (const auto &dependency : manifest.dependencies) {
        const RegistryDependencySource *registry = dependency.Registry();
        if (registry == nullptr || (target && !dependency.MatchesTarget(target->Os()))) {
            continue;
        }
        requirements.push_back(
            PackageRequirement{.ns = registry->ns, .package = dependency.package, .range = registry->version});
    }
    return requirements;
}

PackageResolver::PackageResolver(std::string registryBase, PackageIndexFetcher inputFetchIndex)
    : base(std::move(registryBase))
    , fetchIndex(std::move(inputFetchIndex)) {
}

const std::string &PackageResolver::Base() const noexcept {
    return base;
}

std::expected<const RegistryIndexEntry *, ResolutionFailure> PackageResolver::Get(const IdentitySegment &ns,
                                                                                  const IdentitySegment &package) {
    const std::string key = IdentityKey(ns, package);
    if (const auto found = entries.find(key); found != entries.end()) {
        return &found->second;
    }

    auto fetched = fetchIndex(base, ns, package);
    if (!fetched) {
        return std::unexpected(ResolutionFailure{
            .message = Describe(fetched.error(), base, QualifiedIdentity(ns, package)), .details = {}});
    }
    return &entries.emplace(key, std::move(*fetched)).first->second;
}

std::expected<std::vector<ResolvedPackage>, ResolutionFailure>
PackageResolver::Resolve(const std::span<const PackageRequirement> seeds, const Target::TargetTriple target,
                         const SemanticVersion &compiler) {
    std::map<std::string, Selection> selections;
    std::vector<PackageRequirement> queue(seeds.begin(), seeds.end());
    std::unordered_set<std::string> processed;
    // The CLI reports this insertion order, so seeds and dependencies remain in
    // deterministic breadth-first discovery order.
    std::vector<std::string> order;

    for (std::size_t i = 0; i < queue.size(); ++i) {
        const PackageRequirement requirement = queue[i];
        const std::string key = IdentityKey(requirement.ns, requirement.package);
        if (!processed.insert(key + "@" + requirement.range.Text()).second) {
            continue;
        }

        auto entryResult = Get(requirement.ns, requirement.package);
        if (!entryResult) {
            return std::unexpected(std::move(entryResult.error()));
        }
        const RegistryIndexEntry &entry = **entryResult;

        auto [selection, inserted] = selections.try_emplace(
            key, Selection{.ns = entry.ns, .package = entry.package, .ranges = {}, .version = {}});
        if (inserted) {
            order.push_back(key);
        }
        selection->second.ranges.push_back(requirement.range);

        const RegistryVersion *chosen = SelectVersion(entry, selection->second.ranges, compiler);
        if (chosen == nullptr) {
            return std::unexpected(DescribeResolutionFailure(entry, selection->second.ranges, compiler, base));
        }
        if (!selection->second.version.Empty() && selection->second.version.Text() == chosen->version.Text()) {
            continue;
        }
        selection->second.version = chosen->version;
        for (const auto &edge : chosen->dependencies) {
            if (edge.MatchesTarget(target.Os())) {
                queue.push_back(PackageRequirement{.ns = edge.ns, .package = edge.package, .range = edge.range});
            }
        }
    }

    std::vector<ResolvedPackage> resolved;
    resolved.reserve(order.size());
    for (const auto &key : order) {
        const Selection &selection = selections.at(key);
        resolved.push_back(
            ResolvedPackage{.ns = selection.ns, .package = selection.package, .version = selection.version});
    }
    return resolved;
}
} // namespace Rux::Driver
