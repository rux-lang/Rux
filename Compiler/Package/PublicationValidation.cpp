#include "Package/PublicationValidation.h"

#include <format>

namespace Rux {
std::vector<PackageProblem> ValidateForPublication(const Manifest &manifest) {
    std::vector<PackageProblem> rejections;

    if (manifest.IsWorkspace()) {
        rejections.push_back({"a workspace cannot be published",
                              {"[Workspace] describes a collection, not a package"},
                              "select a member package manifest"});
        return rejections;
    }

    if (manifest.package.type != ManifestPackageType::SourceLibrary) {
        rejections.push_back(
            {std::format("package type '{}' cannot be published by Rux 0.4.0", ToString(manifest.package.type)),
             {"[Package].Type must be 'SourceLibrary' for publication"},
             {}});
    }

    if (!manifest.package.ns) {
        rejections.push_back({"package has no publication namespace",
                              {"[Package].Namespace is required; namespace-free packages are local-only"},
                              {}});
    }

    if (!manifest.header.minRux) {
        rejections.push_back(
            {"package has no minimum supported Rux version",
             {std::format("[Manifest].MinRux is required and must be at least {}", publicationMinRuxFloor)},
             {}});
    }
    else if (const auto floor = SemanticVersion::Parse(publicationMinRuxFloor);
             floor && SemanticVersion::ComparePrecedence(*manifest.header.minRux, *floor) < 0) {
        rejections.push_back(
            {std::format("minimum Rux version '{}' is too old for publication", manifest.header.minRux->Text()),
             {std::format("[Manifest].MinRux must be at least {}", publicationMinRuxFloor)},
             {}});
    }

    for (const auto &dependency : manifest.dependencies) {
        if (dependency.IsPath()) {
            rejections.push_back(
                {std::format("dependency '{}' uses local path '{}'", dependency.importName.Text(), dependency.Path()),
                 {"published packages may depend only on registry packages"},
                 {}});
        }
    }

    return rejections;
}
} // namespace Rux
