#include "Package/PublicationValidation.h"

#include <format>

namespace Rux {
std::vector<std::string> ValidateForPublication(const Manifest &manifest) {
    std::vector<std::string> rejections;

    if (manifest.IsWorkspace()) {
        rejections.emplace_back("a workspace cannot be published; publish a member package instead");
        return rejections;
    }

    if (manifest.package.type != ManifestPackageType::SourceLibrary) {
        rejections.emplace_back(std::format("[Package].Type = \"{}\" cannot be published by Rux 0.4.0; this release "
                                            "publishes only Type = \"SourceLibrary\"",
                                            ToString(manifest.package.type)));
    }

    if (!manifest.package.ns) {
        rejections.emplace_back("publication requires [Package].Namespace; a namespace-free package is local-only");
    }

    if (!manifest.header.minRux) {
        rejections.emplace_back(
            std::format("publication requires [Manifest].MinRux, the oldest compiler release that can build the "
                        "package; it must be at least {}",
                        publicationMinRuxFloor));
    }
    else if (const auto floor = SemanticVersion::Parse(publicationMinRuxFloor);
             floor && SemanticVersion::ComparePrecedence(*manifest.header.minRux, *floor) < 0) {
        rejections.emplace_back(std::format("[Manifest].MinRux is '{}' but publication requires at least {}",
                                            manifest.header.minRux->Text(), publicationMinRuxFloor));
    }

    for (const auto &dependency : manifest.dependencies) {
        if (dependency.IsPath()) {
            rejections.emplace_back(
                std::format("dependency '{}' uses Path = \"{}\"; publication requires registry dependencies",
                            dependency.importName.Text(), dependency.Path()));
        }
    }

    return rejections;
}
} // namespace Rux
