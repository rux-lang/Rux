// The legacy Result and Option protocols, audited across everything the workspace maintains.
//
// `rux check --deny-legacy-protocols` reports each `?` and `??` the compiler still resolves by a variant's case names,
// and each iterator whose `Next` returns such a variant, as an error at its site. This suite runs that audit over
// every workspace package, every first-party README block, and every Tests/Language and Tests/Packages fixture, and
// compares the sites per file with LegacyProtocolAuditBaseline.txt. The baseline records what remains to migrate:
// it may only shrink, and the package migration gate is passed when it is empty. The fixtures that exist to test the
// legacy protocols themselves are excluded; tasks 36-37 migrate or remove them together with the protocols.
//
// To regenerate the baseline after a migration, run the test binary with RUX_UPDATE_GOLDEN=1 and review the diff.

#include "PackageExampleSupport.h"
#include "SemanticTestSupport.h"
#include "System/Os.h"
#include "System/Process.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace Rux;

namespace {

/// The fixtures whose subject is a legacy protocol. They keep it working until tasks 36-37 remove it.
constexpr std::array ProtocolFixtures{
    std::string_view("Tests/Language/Coalescing"),  std::string_view("Tests/Language/Iteration"),
    std::string_view("Tests/Language/Propagation"), std::string_view("Tests/Language/PropagationPayload"),
    std::string_view("Tests/Language/Try"),
};

/// One `rux check` run, and the name its sites are recorded under when the source it checks is not a file of the
/// repository.
struct AuditJob {
    std::filesystem::path manifest;
    std::string_view target;
    std::string scratchLabel; ///< set for a README block, whose scratch source stands for the README
};

std::filesystem::path RootDir() {
    return std::filesystem::weakly_canonical(std::filesystem::path(RUX_ROOT_DIR));
}

std::filesystem::path BaselinePath() {
    return RootDir() / "Tests" / "Unit" / "LegacyProtocolAuditBaseline.txt";
}

std::string Relative(const std::filesystem::path &path) {
    return std::filesystem::path(path).lexically_relative(RootDir()).generic_string();
}

bool IsProtocolFixture(const std::filesystem::path &directory) {
    return std::ranges::contains(ProtocolFixtures, std::string_view(Relative(directory)));
}

/// Every fixture package under Tests/Language and Tests/Packages, in a stable order.
std::vector<std::filesystem::path> FixtureManifests() {
    std::vector<std::filesystem::path> manifests;
    for (const auto *tree : {"Language", "Packages"}) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(RootDir() / "Tests" / tree)) {
            if (entry.is_regular_file() && entry.path().filename() == "Rux.toml" &&
                !IsProtocolFixture(entry.path().parent_path())) {
                manifests.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(manifests);
    return manifests;
}

/// The platform packages the host does not check as workspace members, each with a target that selects it.
constexpr std::array PlatformPackages{
    std::pair{std::string_view("FreeBSD"), std::string_view("freebsd-x86_64")},
    std::pair{std::string_view("Linux"), std::string_view("linux-x86_64")},
    std::pair{std::string_view("macOS"), std::string_view("macos-x86_64")},
    std::pair{std::string_view("Windows"), std::string_view("windows-x86_64")},
};

/// The audited sites of one run's output, each named by the file it was reported in. A site seen by two runs, such as
/// a platform package checked both as a workspace member and for its own target, is one site.
void CollectSites(const AuditJob &job, const std::string &output,
                  std::set<std::pair<std::string, std::string>> &sites) {
    static const std::regex diagnostic(R"(^(.+):(\d+):(\d+): error: (.+ uses the legacy .+)$)");
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::smatch match;
        if (!std::regex_match(line, match, diagnostic)) {
            continue;
        }
        const std::string file = job.scratchLabel.empty() ? Relative(match[1].str()) : job.scratchLabel;
        sites.emplace(file, std::format("{}:{}:{}: {}", file, match[2].str(), match[3].str(), match[4].str()));
    }
}

/// The audit errors among `diagnostics`.
std::vector<std::string> AuditErrors(const std::vector<SemanticDiagnostic> &diagnostics) {
    std::vector<std::string> errors;
    for (const auto &diagnostic : diagnostics) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error && diagnostic.message.contains("legacy")) {
            errors.push_back(
                std::format("{}:{}: {}", diagnostic.location.line, diagnostic.location.column, diagnostic.message));
        }
    }
    return errors;
}

constexpr std::string_view LegacyProtocols = R"(
    variant Result<T, E> { Success(T), Error(E) }
    variant Option<T> { Some(T), None }
)";

std::map<std::string, std::size_t> LoadBaseline() {
    std::map<std::string, std::size_t> entries;
    std::ifstream input(BaselinePath());
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto separator = line.find('|');
        REQUIRE_MESSAGE(separator != std::string::npos, "malformed baseline entry '", line, "'");
        entries[line.substr(separator + 1)] = std::stoul(line.substr(0, separator));
    }
    return entries;
}

void WriteBaseline(const std::map<std::string, std::size_t> &counts) {
    std::ofstream output(BaselinePath(), std::ios::binary);
    output << "# Legacy Result and Option protocol sites that remain to migrate, counted per file.\n"
              "# Format: count|file   A README block is named as its README plus '#' and its position.\n"
              "# This file may only shrink; the package migration gate is passed when no entry remains.\n"
              "# Regenerate with: RUX_UPDATE_GOLDEN=1 rux-tests --source-file='*LegacyProtocolAuditTests.cpp'\n";
    for (const auto &[file, count] : counts) {
        output << count << '|' << file << '\n';
    }
}

} // namespace

TEST_CASE("the audit reports each legacy protocol site once and changes nothing without the switch") {
    const std::string source = std::string(LegacyProtocols) + R"(
        struct E {}
        struct Counter { n: int32; }
        extend Counter {
            func Next(self: &var Counter) -> Option<int32> { return Option::None<int32>(); }
        }
        func Read() -> Result<int32, E> { return Result::Success<int32, E>(1i32); }
        func Use() -> Result<int32, E> {
            let value = Read()?;
            return Result::Success<int32, E>(value);
        }
        func Find() -> Option<int32> { return Option::Some<int32>(2i32); }
        func Forward() -> Option<int32> { return Option::Some<int32>(Find()?); }
        func Pick() -> int32 { return Find() ?? 0i32; }
        func Fallback<T>(value: Option<T>, fallback: T) -> T { return value ?? fallback; }
        func Loop(counter: Counter) -> int32 {
            var total = 0i32;
            for value in counter {
                total = total + value;
            }
            return total + Fallback<int32>(Find(), 1i32) + (Fallback<bool>(Option::Some<bool>(true), false) ? 1i32 : 0i32);
        }
        func Native(value: int32 ! E, optional: int32?) -> int32 ! E {
            return value? + (optional ?? 0i32);
        }
    )";
    CHECK(AuditErrors(Testing::SemanticTestSupport::AnalyzeSource(source)).empty());

    const auto errors = AuditErrors(Testing::SemanticTestSupport::AnalyzeSource(source, {.denyLegacyProtocols = true}));
    INFO(std::ranges::fold_left(errors, std::string(), [](std::string all, const std::string &error) {
        return std::move(all) + error + "\n";
    }));
    REQUIRE_EQ(errors.size(), 6);
    CHECK(errors[0].contains("iterator method 'Next' on 'Counter' uses the legacy Option protocol"));
    CHECK(
        errors[1].contains("'?' uses the legacy Result protocol on 'Result<int32, E>'; migrate to a native fallible"));
    CHECK(errors[2].contains("'?' uses the legacy Option protocol on 'Option<int32>'; migrate to a native optional"));
    CHECK(errors[3].contains("'?"
                             "?' uses the legacy Option protocol on 'Option<int32>'"));
    // The generic body is analyzed for each instantiation but reported once.
    CHECK(errors[4].contains("'?"
                             "?' uses the legacy Option protocol on 'Option<T>'"));
    CHECK(errors[5].contains("'for' uses the legacy Option protocol of 'Option<int32>'"));
}

TEST_CASE("the audit reports only the package being analyzed") {
    const std::string dependency = std::string(LegacyProtocols) + R"(
        pub func Find() -> Option<int32> { return Option::Some<int32>(2i32); }
        pub func Pick() -> int32 { return Find() ?? 0i32; }
        pub func Fallback<T>(value: Option<T>, fallback: T) -> T { return value ?? fallback; }
    )";
    const auto errors =
        AuditErrors(Testing::SemanticTestSupport::AnalyzeWithDep(R"(
        import Store::{ Fallback, Find, Pick };
        func Use() -> int32 {
            return Pick() + Fallback<int32>(Find(), 1i32);
        }
    )",
                                                                 "Store", dependency, {.denyLegacyProtocols = true}));
    CHECK(errors.empty());
}

TEST_CASE("the legacy protocol audit lists exactly the recorded baseline") {
    const auto compiler = RootDir() / "Bin" / System::ExecutableFileName("rux");
    REQUIRE_MESSAGE(std::filesystem::exists(compiler), "the compiler has to be built before the workspace is audited");
    for (const std::string_view fixture : ProtocolFixtures) {
        CHECK_MESSAGE(std::filesystem::exists(RootDir() / fixture / "Rux.toml"), fixture,
                      " is no longer a fixture. Remove it from ProtocolFixtures.");
    }

    std::vector<AuditJob> jobs;
    jobs.push_back({RootDir() / "Rux.toml", {}, {}});
    for (const auto &[package, target] : PlatformPackages) {
        jobs.push_back({RootDir() / "Packages" / package / "Rux.toml", target, {}});
    }
    for (const auto &manifest : FixtureManifests()) {
        jobs.push_back({manifest, {}, {}});
    }
    std::size_t signatures = 0;
    const auto examples = Testing::PackageExamples::MaterializeReadmeExamples(
        std::filesystem::path(RUX_TEST_BIN_DIR) / "rux-legacy-protocol-audit", signatures);
    for (const auto &example : examples) {
        if (!example.fragment) {
            jobs.push_back({example.manifest, example.target,
                            std::format("Packages/{}/README.md#{}", example.package, example.block)});
        }
    }

    // Each run is an independent process over its own package, so the runs share nothing but the result.
    std::set<std::pair<std::string, std::string>> sites;
    std::vector<std::string> failures;
    std::mutex results;
    std::atomic<std::size_t> next = 0;
    const auto worker = [&] {
        for (std::size_t index = next++; index < jobs.size(); index = next++) {
            const AuditJob &job = jobs[index];
            const auto manifest = job.manifest.string();
            std::vector<std::string_view> arguments{"--manifest", manifest, "--color=never", "check",
                                                    "--deny-legacy-protocols"};
            if (!job.target.empty()) {
                arguments.emplace_back("--target");
                arguments.push_back(job.target);
            }
            const auto result = System::RunCaptured(compiler, arguments);
            const std::scoped_lock lock(results);
            if (!result) {
                failures.push_back(std::format("could not launch the compiler for {}", manifest));
                continue;
            }
            CollectSites(job, result->output, sites);
        }
    };
    std::vector<std::jthread> workers;
    const std::size_t parallelism = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8);
    for (std::size_t index = 0; index < parallelism; ++index) {
        workers.emplace_back(worker);
    }
    workers.clear();
    for (const auto &failure : failures) {
        FAIL_CHECK(failure);
    }

    std::filesystem::remove_all(std::filesystem::path(RUX_TEST_BIN_DIR) / "rux-legacy-protocol-audit");
    std::map<std::string, std::size_t> counts;
    std::string listing;
    for (const auto &[file, site] : sites) {
        ++counts[file];
        listing += site + "\n";
    }
    MESSAGE("audited ", jobs.size(), " packages; ", sites.size(), " legacy protocol sites remain in ", counts.size(),
            " files");

    if (System::HasEnv("RUX_UPDATE_GOLDEN")) {
        WriteBaseline(counts);
        return;
    }
    const auto baseline = LoadBaseline();
    std::string drift;
    for (const auto &[file, count] : counts) {
        const auto recorded = baseline.find(file);
        if (recorded == baseline.end() || recorded->second != count) {
            drift += std::format("  {}: {} sites, baseline {}\n", file, count,
                                 recorded == baseline.end() ? 0 : recorded->second);
        }
    }
    for (const auto &[file, count] : baseline) {
        if (!counts.contains(file)) {
            drift += std::format("  {}: 0 sites, baseline {}\n", file, count);
        }
    }
    INFO("remaining sites:\n", listing);
    CHECK_MESSAGE(
        drift.empty(), "the legacy protocol sites differ from LegacyProtocolAuditBaseline.txt:\n", drift,
        "A migration shrinks the baseline: regenerate it with RUX_UPDATE_GOLDEN=1. A new legacy use should be "
        "written with the native forms instead.");
}
