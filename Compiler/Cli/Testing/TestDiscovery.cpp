#include "Cli/Testing/TestPackages.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

namespace Rux::CliSupport {
bool SourcesDeclareTests(const std::filesystem::path &packageDir) {
    std::error_code ec;
    const auto sources = packageDir / "Src";
    if (!std::filesystem::is_directory(sources, ec)) {
        return false;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(sources, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".rux") {
            continue;
        }
        std::ifstream file(it->path(), std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (text.find("#Test(") != std::string::npos) {
            return true;
        }
    }
    return false;
}

TestDiscovery DiscoverTestPackages(const std::span<const TestRoot> roots) {
    TestDiscovery result;
    {
        std::error_code ec;
        constexpr int maxGroupDepth = 3;
        for (const auto &root : roots) {
            if (!std::filesystem::exists(root.dir, ec)) {
                continue;
            }
            result.anyRootExists = true;
            std::vector<std::pair<std::filesystem::path, int>> pendingDirs;
            pendingDirs.emplace_back(root.dir, 0);
            while (!pendingDirs.empty()) {
                const auto [dir, depth] = std::move(pendingDirs.back());
                pendingDirs.pop_back();
                for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (!entry.is_directory()) {
                        continue;
                    }
                    const auto toml = entry.path() / "Rux.toml";
                    if (!std::filesystem::exists(toml)) {
                        if (depth + 1 < maxGroupDepth) {
                            pendingDirs.emplace_back(entry.path(), depth + 1);
                        }
                        continue;
                    }
                    auto pkgManifest = Manifest::Load(toml);
                    if (!pkgManifest.Ok()) {
                        for (const auto &diagnostic : pkgManifest.diagnostics) {
                            result.diagnostics.push_back(diagnostic);
                        }
                        continue;
                    }
                    // An Executable package has an entry point to run; any other package runs only when a test build
                    // can generate one for its '#Test()' functions.
                    if (pkgManifest.manifest->package.type != ManifestPackageType::Executable &&
                        !SourcesDeclareTests(entry.path())) {
                        continue;
                    }
                    auto label = entry.path().lexically_relative(root.dir).generic_string();
                    if (!root.labelPrefix.empty()) {
                        label = root.labelPrefix + "/" + label;
                    }
                    result.packages.push_back({entry.path(), std::move(label), std::move(*pkgManifest.manifest)});
                }
            }
        }
        std::sort(result.packages.begin(), result.packages.end(),
                  [](const TestPackage &a, const TestPackage &b) { return a.label < b.label; });
    }
    return result;
}
} // namespace Rux::CliSupport
