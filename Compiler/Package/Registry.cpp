#include "Package/Registry.h"

#include "BuildInfo/CompilerMetadata.h"
#include "Package/Manifest.h"
#include "System/Http.h"
#include "System/Json.h"
#include "System/Process.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>
#include <span>
#include <utility>

using namespace Rux::System;

namespace Rux::Packages {
namespace {
/// Build one `/v1` URL from already-normalized identity segments.
std::string RouteUrl(const std::string_view base, const std::string_view suffix) {
    return std::string(base) + std::string(suffix);
}

std::string PackageRoute(const IdentitySegment &ns, const IdentitySegment &package) {
    return UrlEncode(ns.Normalized()) + "/" + UrlEncode(package.Normalized());
}

RegistryError Malformed(std::string detail) {
    return RegistryError{.kind = RegistryErrorKind::Malformed, .status = 0, .code = {}, .detail = std::move(detail)};
}

/// Read one identity segment out of a response, preserving its spelling.
std::optional<IdentitySegment> ReadSegment(const JsonValue &object, const std::string_view key) {
    auto parsed = IdentitySegment::Parse(object.StringAt(key));
    if (!parsed) {
        return std::nullopt;
    }
    return std::move(*parsed);
}

/**
 * @brief Perform one unauthenticated GET and separate success from a problem.
 *
 * A 404 becomes NotFound whatever the body says, because the two package
 * routes report absence with different problem codes and every caller means the
 * same thing by them.
 */
std::expected<std::string, RegistryError> GetRoute(const std::string_view url) {
    std::string failureDetail;
    auto response = HttpSend({.method = "GET", .url = std::string(url), .headers = {}, .body = {}}, &failureDetail);
    if (!response) {
        return std::unexpected(RegistryError{
            .kind = RegistryErrorKind::Unreachable, .status = 0, .code = {}, .detail = std::move(failureDetail)});
    }
    if (response->status >= 200 && response->status < 300) {
        return std::move(response->body);
    }
    RegistryError error = DecodeProblem(response->body, response->status);
    if (response->status == 404) {
        error.kind = RegistryErrorKind::NotFound;
    }
    return std::unexpected(std::move(error));
}
} // namespace

bool RegistryDependencyEdge::MatchesTarget(const Target::OS os) const noexcept {
    return targetOS.empty() || std::ranges::contains(targetOS, os);
}

std::string QualifiedIdentity(const IdentitySegment &ns, const IdentitySegment &package) {
    return std::format("{}/{}", ns.Text(), package.Text());
}

RegistryError DecodeProblem(const std::string_view body, const unsigned status) {
    RegistryError error{.kind = RegistryErrorKind::Rejected, .status = status, .code = {}, .detail = {}};
    const auto document = ParseJson(body);
    if (!document) {
        return error;
    }
    error.code = document->StringAt("code");
    if (error.code.size() > 64 || !std::ranges::all_of(error.code, [](const unsigned char ch) {
            return std::isalnum(ch) != 0 || ch == '_' || ch == '-';
        })) {
        error.code.clear();
    }
    return error;
}

ResolutionFailure Describe(const RegistryError &error, const std::string_view base, const std::string_view identity) {
    const std::string registry = SanitizeUrl(base);
    ResolutionFailure failure;
    switch (error.kind) {
    case RegistryErrorKind::Unreachable:
        failure.message = std::format("could not connect to registry '{}'", registry);
        if (!error.detail.empty()) {
            failure.notes.push_back(error.detail);
        }
        failure.help = "check the registry URL and network connection, then retry";
        return failure;
    case RegistryErrorKind::NotFound:
        failure.message = std::format("package '{}' is not available from registry '{}'", identity, registry);
        failure.notes.push_back(std::format("registry response status: {}", error.status));
        failure.help = "check the package identity and requested registry";
        return failure;
    case RegistryErrorKind::Malformed:
        failure.message =
            std::format("registry '{}' returned an invalid response for package '{}'", registry, identity);
        if (!error.detail.empty()) {
            failure.notes.push_back("response decoding failed: " + error.detail);
        }
        failure.help = "retry the request; contact the registry operator if the response remains invalid";
        return failure;
    case RegistryErrorKind::Rejected:
        break;
    }
    failure.message = std::format("registry '{}' rejected the request for package '{}'", registry, identity);
    if (error.status != 0) {
        failure.notes.push_back(std::format("registry response status: {}", error.status));
    }
    if (error.code == "rate_limited") {
        failure.help = "wait briefly, then retry the request";
    }
    else {
        failure.help = "check the registry status and retry the request";
    }
    return failure;
}

std::expected<RegistryIndexEntry, RegistryError> DecodePackageIndex(const std::string_view body) {
    const auto document = ParseJson(body);
    if (!document) {
        return std::unexpected(Malformed(document.error().message));
    }
    const JsonValue *data = document->Find("data");
    if (data == nullptr || !data->IsObject()) {
        return std::unexpected(Malformed("the index has no 'data' object"));
    }

    auto ns = ReadSegment(*data, "namespace");
    auto package = ReadSegment(*data, "package");
    if (!ns || !package) {
        return std::unexpected(Malformed("the index does not name a valid package identity"));
    }

    RegistryIndexEntry entry{.ns = std::move(*ns), .package = std::move(*package), .versions = {}};
    const JsonValue *versions = data->Find("versions");
    if (versions == nullptr || !versions->IsArray()) {
        return std::unexpected(Malformed("the index has no 'versions' array"));
    }

    for (const auto &element : versions->Elements()) {
        auto version = SemanticVersion::Parse(element.StringAt("version"));
        if (!version) {
            return std::unexpected(
                Malformed(std::format("the index lists an unreadable version '{}'", element.StringAt("version"))));
        }
        RegistryVersion published{.version = std::move(*version),
                                  .minRux = std::nullopt,
                                  .yanked = element.BoolAt("yanked"),
                                  .dependencies = {}};

        // A registry that does not record a minimum leaves the member null,
        // which is not the same as recording an unreadable one.
        if (const JsonValue *minRux = element.Find("min_rux"); minRux != nullptr && !minRux->IsNull()) {
            auto parsed = SemanticVersion::Parse(minRux->AsString());
            if (!parsed) {
                return std::unexpected(
                    Malformed(std::format("the index lists an unreadable MinRux '{}'", minRux->AsString())));
            }
            published.minRux = std::move(*parsed);
        }

        if (const JsonValue *dependencies = element.Find("dependencies"); dependencies != nullptr) {
            for (const auto &edge : dependencies->Elements()) {
                auto alias = ReadSegment(edge, "alias");
                auto targetNs = ReadSegment(edge, "target_namespace");
                auto targetPackage = ReadSegment(edge, "target_package");
                auto range = VersionRange::Parse(edge.StringAt("version_range"));
                if (!alias || !targetNs || !targetPackage || !range) {
                    return std::unexpected(Malformed(std::format(
                        "the index lists an unreadable dependency of version {}", published.version.Text())));
                }
                RegistryDependencyEdge dependency{.alias = std::move(*alias),
                                                  .ns = std::move(*targetNs),
                                                  .package = std::move(*targetPackage),
                                                  .range = std::move(*range),
                                                  .targetOS = {}};
                if (const JsonValue *targetOS = edge.Find("target_os"); targetOS != nullptr) {
                    if (!targetOS->IsArray() || targetOS->Elements().empty()) {
                        return std::unexpected(Malformed(
                            std::format("the index lists an unreadable target_os for a dependency of version {}",
                                        published.version.Text())));
                    }
                    for (const auto &item : targetOS->Elements()) {
                        const auto os = ParseManifestTargetOS(item.AsString());
                        if (!os || std::ranges::contains(dependency.targetOS, *os)) {
                            return std::unexpected(Malformed(
                                std::format("the index lists an unreadable target_os for a dependency of version {}",
                                            published.version.Text())));
                        }
                        dependency.targetOS.push_back(*os);
                    }
                }
                published.dependencies.push_back(std::move(dependency));
            }
        }
        entry.versions.push_back(std::move(published));
    }
    return entry;
}

std::expected<std::string, RegistryError> DecodeArtifactChecksum(const std::string_view body) {
    const auto document = ParseJson(body);
    if (!document) {
        return std::unexpected(Malformed(document.error().message));
    }
    const JsonValue *data = document->Find("data");
    if (data == nullptr || !data->IsObject()) {
        return std::unexpected(Malformed("the version metadata has no 'data' object"));
    }
    const JsonValue *checksum = data->Find("checksum");
    if (checksum == nullptr || !checksum->IsObject()) {
        return std::unexpected(Malformed("the version metadata carries no checksum"));
    }
    if (const std::string &algorithm = checksum->StringAt("algorithm"); algorithm != "sha256") {
        return std::unexpected(Malformed(std::format("the artifact checksum uses '{}' rather than sha256", algorithm)));
    }
    const std::string &digest = checksum->StringAt("digest");
    if (digest.empty()) {
        return std::unexpected(Malformed("the artifact checksum has no digest"));
    }
    return digest;
}

std::expected<RegistryIndexEntry, RegistryError>
FetchPackageIndex(const std::string_view base, const IdentitySegment &ns, const IdentitySegment &package) {
    auto body = GetRoute(RouteUrl(base, "/v1/index/" + PackageRoute(ns, package)));
    if (!body) {
        return std::unexpected(std::move(body.error()));
    }
    return DecodePackageIndex(*body);
}

std::expected<std::string, RegistryError> FetchArtifactChecksum(const std::string_view base, const IdentitySegment &ns,
                                                                const IdentitySegment &package,
                                                                const SemanticVersion &version) {
    auto body = GetRoute(RouteUrl(base, "/v1/packages/" + PackageRoute(ns, package) + "/" + UrlEncode(version.Text())));
    if (!body) {
        return std::unexpected(std::move(body.error()));
    }
    return DecodeArtifactChecksum(*body);
}

std::expected<std::string, RegistryError> DownloadArtifact(const std::string_view base, const IdentitySegment &ns,
                                                           const IdentitySegment &package,
                                                           const SemanticVersion &version) {
    // The route answers 307 with a CDN location; HttpSend follows it, so the
    // body here is already the artifact rather than an empty redirect.
    auto body = GetRoute(
        RouteUrl(base, "/v1/packages/" + PackageRoute(ns, package) + "/" + UrlEncode(version.Text()) + "/download"));
    if (!body) {
        return std::unexpected(std::move(body.error()));
    }
    if (body->empty()) {
        return std::unexpected(Malformed("the download route returned no artifact"));
    }
    return body;
}

VersionRejection ClassifyVersion(const RegistryVersion &candidate, const std::span<const VersionRange> ranges,
                                 const SemanticVersion &compiler) {
    const bool matchesAll = std::ranges::all_of(
        ranges, [&candidate](const VersionRange &range) { return range.Matches(candidate.version); });
    if (!matchesAll) {
        return VersionRejection::RequirementUnmet;
    }
    if (candidate.yanked) {
        return VersionRejection::Yanked;
    }
    if (candidate.minRux && SemanticVersion::ComparePrecedence(*candidate.minRux, compiler) > 0) {
        return VersionRejection::CompilerTooOld;
    }
    return VersionRejection::Eligible;
}

const RegistryVersion *SelectVersion(const RegistryIndexEntry &entry, const std::span<const VersionRange> ranges,
                                     const SemanticVersion &compiler) {
    const RegistryVersion *best = nullptr;
    for (const auto &candidate : entry.versions) {
        if (ClassifyVersion(candidate, ranges, compiler) != VersionRejection::Eligible) {
            continue;
        }
        // Precedence ignores build metadata, so two candidates can tie. The
        // index is ascending and uses build metadata as its own tie-break, so
        // taking the later of a tie matches the registry's total order.
        if (best == nullptr || SemanticVersion::ComparePrecedence(candidate.version, best->version) >= 0) {
            best = &candidate;
        }
    }
    return best;
}

const RegistryVersion *SelectVersion(const RegistryIndexEntry &entry, const VersionRange &range,
                                     const SemanticVersion &compiler) {
    return SelectVersion(entry, std::span(&range, 1), compiler);
}

std::string DescribeRanges(const std::span<const VersionRange> ranges) {
    std::string listed;
    for (const auto &range : ranges) {
        listed += (listed.empty() ? "'" : ", '") + range.Text() + "'";
    }
    return listed;
}

ResolutionFailure DescribeResolutionFailure(const RegistryIndexEntry &entry, const std::span<const VersionRange> ranges,
                                            const SemanticVersion &compiler, const std::string_view base) {
    const std::string identity = QualifiedIdentity(entry.ns, entry.package);

    // Highest first: the version the user most likely expected to get is the
    // one they should read about first.
    std::vector<const RegistryVersion *> excluded;
    for (const auto &candidate : std::ranges::reverse_view(entry.versions)) {
        if (const VersionRejection rejection = ClassifyVersion(candidate, ranges, compiler);
            rejection != VersionRejection::Eligible && rejection != VersionRejection::RequirementUnmet) {
            excluded.push_back(&candidate);
        }
    }

    /// One excluded version, phrased to stand on its own.
    const auto reason = [&identity, &compiler](const RegistryVersion &candidate) {
        if (candidate.yanked) {
            return std::format("package '{}' version '{}' has been yanked", identity, candidate.version.Text());
        }
        return std::format("package '{}' version '{}' requires Rux '{}' or newer, but this is Rux '{}'", identity,
                           candidate.version.Text(), candidate.minRux->Text(), compiler.Text());
    };

    if (excluded.size() == 1) {
        // The single excluded version is the whole story: leading with the
        // requirement would only delay it by a line.
        return ResolutionFailure{.message = reason(*excluded.front()),
                                 .notes = {},
                                 .help = "select a version that is eligible for this compiler"};
    }

    ResolutionFailure failure{.message =
                                  std::format("no version of '{}' satisfies {}", identity, DescribeRanges(ranges)),
                              .notes = {},
                              .help = "change the dependency requirements to select a compatible published version"};
    if (excluded.empty()) {
        failure.notes.push_back(
            std::format("registry '{}' publishes {}", SanitizeUrl(base), DescribeAvailableVersions(entry)));
        return failure;
    }
    for (const RegistryVersion *candidate : excluded) {
        failure.notes.push_back(reason(*candidate));
    }
    return failure;
}

std::string DescribeAvailableVersions(const RegistryIndexEntry &entry) {
    std::string listed;
    for (const auto &candidate : entry.versions) {
        if (!listed.empty()) {
            listed += ", ";
        }
        listed += candidate.version.Text();
        if (candidate.yanked) {
            listed += " (yanked)";
        }
    }
    return listed.empty() ? std::string("none") : listed;
}

SemanticVersion CompilerVersion() {
    auto parsed = SemanticVersion::Parse(CompilerBuild::compilerVersion);
    return parsed ? std::move(*parsed) : SemanticVersion{};
}
} // namespace Rux::Packages
