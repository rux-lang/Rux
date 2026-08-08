#include "Driver/Credentials.h"
#include "System/Os.h"
#include "Target/Target.h"

#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using namespace Rux::Driver;
using namespace Rux::System;
using namespace Rux::Target;

namespace {
/// The variable UserDataDir derives the per-user root from on this host.
constexpr const char *homeVariable = (HostOS == OS::Windows) ? "LOCALAPPDATA" : "HOME";

/**
 * @brief Point the per-user directory at a private temporary tree.
 *
 * Every case here writes a credentials file, so without this they would
 * overwrite the credentials of whoever is running the tests. The previous
 * directory and RUX_TOKEN are both restored on the way out.
 */
class ScopedUserDataDir {
public:
    ScopedUserDataDir() {
        static int sequence = 0;
        savedHome_ = GetEnvPath(homeVariable);
        savedToken_ = GetEnv(kCredentialVariable);
        savedRegistry_ = GetEnv(kRegistryVariable);

        root_ = TempDirectory() / ("RuxCredentialsTest-" + std::to_string(++sequence));
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_, ec);
        REQUIRE(!ec);
        REQUIRE(SetEnvPath(homeVariable, root_));
        REQUIRE(UnsetEnv(kCredentialVariable));
        // A redirect that silently failed would send the writes below at the
        // real credentials file, so prove the path moved before any test runs.
        REQUIRE(CredentialsPath().string().starts_with(root_.string()));
    }

    ScopedUserDataDir(const ScopedUserDataDir &) = delete;
    ScopedUserDataDir &operator=(const ScopedUserDataDir &) = delete;

    ~ScopedUserDataDir() {
        if (savedHome_) {
            static_cast<void>(SetEnvPath(homeVariable, *savedHome_));
        }
        else {
            static_cast<void>(UnsetEnv(homeVariable));
        }
        if (savedToken_) {
            static_cast<void>(SetEnv(kCredentialVariable, *savedToken_));
        }
        else {
            static_cast<void>(UnsetEnv(kCredentialVariable));
        }
        if (savedRegistry_) {
            static_cast<void>(SetEnv(kRegistryVariable, *savedRegistry_));
        }
        else {
            static_cast<void>(UnsetEnv(kRegistryVariable));
        }
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

private:
    std::optional<std::filesystem::path> savedHome_;
    std::optional<std::string> savedToken_;
    std::optional<std::string> savedRegistry_;
    std::filesystem::path root_;
};

constexpr const char *official = "https://api.rux-lang.dev";
constexpr const char *local = "http://localhost:8080";
} // namespace

TEST_CASE("a stored credential is read back with the file as its source") {
    const ScopedUserDataDir sandbox;

    CHECK_FALSE(LoadCredential(official).has_value());
    REQUIRE(StoreCredential(official, "rux_pat_one").has_value());

    const auto loaded = LoadCredential(official);
    REQUIRE(loaded.has_value());
    CHECK(loaded->token == "rux_pat_one");
    CHECK(loaded->source == CredentialsPath().generic_string());
}

TEST_CASE("each registry keeps its own token") {
    const ScopedUserDataDir sandbox;

    REQUIRE(StoreCredential(official, "official-token").has_value());
    REQUIRE(StoreCredential(local, "local-token").has_value());
    REQUIRE(LoadCredential(official).has_value());
    REQUIRE(LoadCredential(local).has_value());
    CHECK(LoadCredential(official)->token == "official-token");
    CHECK(LoadCredential(local)->token == "local-token");

    // Replacing one entry must leave every other registry's alone: this is what
    // stops a local test registry from being handed the official credential.
    REQUIRE(StoreCredential(local, "local-token-2").has_value());
    CHECK(LoadCredential(official)->token == "official-token");
    CHECK(LoadCredential(local)->token == "local-token-2");
    CHECK_FALSE(LoadCredential("https://registry.example").has_value());
}

TEST_CASE("trailing slashes name the same registry") {
    const ScopedUserDataDir sandbox;

    CHECK(NormalizeRegistryBase("https://api.rux-lang.dev///") == official);
    CHECK(NormalizeRegistryBase("  https://api.rux-lang.dev/  ") == official);

    REQUIRE(StoreCredential("https://api.rux-lang.dev/", "token").has_value());
    REQUIRE(LoadCredential(official).has_value());
    CHECK(LoadCredential(official)->token == "token");

    // The slashed and unslashed forms address one entry, not two.
    REQUIRE(StoreCredential(official, "replacement").has_value());
    REQUIRE(LoadCredential("https://api.rux-lang.dev/").has_value());
    CHECK(LoadCredential("https://api.rux-lang.dev/")->token == "replacement");
    const auto erased = EraseCredential("https://api.rux-lang.dev//");
    REQUIRE(erased.has_value());
    CHECK(*erased);
    CHECK_FALSE(LoadCredential(official).has_value());
}

TEST_CASE("RUX_TOKEN outranks the stored token") {
    const ScopedUserDataDir sandbox;

    REQUIRE(StoreCredential(official, "stored-token").has_value());
    REQUIRE(SetEnv(kCredentialVariable, "env-token"));

    const auto resolved = ResolveCredential(official);
    REQUIRE(resolved.has_value());
    CHECK(resolved->token == "env-token");
    CHECK(resolved->source == kCredentialVariable);

    // The environment only shadows the file; it does not replace it.
    REQUIRE(LoadCredential(official).has_value());
    CHECK(LoadCredential(official)->token == "stored-token");

    // An unset variable falls through to the stored token, and so does an empty
    // one -- an exported-but-blank RUX_TOKEN must not lock publishing out.
    REQUIRE(SetEnv(kCredentialVariable, ""));
    REQUIRE(ResolveCredential(official).has_value());
    CHECK(ResolveCredential(official)->token == "stored-token");

    REQUIRE(UnsetEnv(kCredentialVariable));
    REQUIRE(ResolveCredential(official).has_value());
    CHECK(ResolveCredential(official)->token == "stored-token");
    CHECK(ResolveCredential(official)->source == CredentialsPath().generic_string());
    CHECK_FALSE(ResolveCredential(local).has_value());
}

TEST_CASE("erasing removes one registry and reports whether it was there") {
    const ScopedUserDataDir sandbox;

    REQUIRE(StoreCredential(official, "official-token").has_value());
    REQUIRE(StoreCredential(local, "local-token").has_value());

    const auto first = EraseCredential(official);
    REQUIRE(first.has_value());
    CHECK(*first);
    CHECK_FALSE(LoadCredential(official).has_value());
    CHECK(LoadCredential(local).has_value());

    // Logging out twice is not an error, it just has nothing left to do.
    const auto again = EraseCredential(official);
    REQUIRE(again.has_value());
    CHECK_FALSE(*again);

    // Removing the last entry leaves no file behind.
    REQUIRE(EraseCredential(local).has_value());
    CHECK_FALSE(std::filesystem::exists(CredentialsPath()));
}

TEST_CASE("a missing or malformed credentials file yields no credential") {
    const ScopedUserDataDir sandbox;

    CHECK_FALSE(LoadCredential(official).has_value());

    std::error_code ec;
    std::filesystem::create_directories(CredentialsPath().parent_path(), ec);
    {
        std::ofstream file(CredentialsPath(), std::ios::binary);
        REQUIRE(file.is_open());
        file << "this is not a credentials file\n"
             << "[Registry.\n"
             << "Token = unquoted\n"
             << "[Registry.\"https://api.rux-lang.dev\"]\n"
             << "Nonsense\n";
    }
    // A corrupt file reads as "no credential" rather than throwing, so publish
    // fails with its own actionable message instead of an unexplained crash.
    CHECK_FALSE(LoadCredential(official).has_value());

    // And it is recoverable: `rux login` rewrites the file wholesale.
    REQUIRE(StoreCredential(official, "fresh").has_value());
    REQUIRE(LoadCredential(official).has_value());
    CHECK(LoadCredential(official)->token == "fresh");
}

TEST_CASE("the credentials file is restricted to its owner") {
    const ScopedUserDataDir sandbox;

    REQUIRE(StoreCredential(official, "secret").has_value());
    REQUIRE(std::filesystem::exists(CredentialsPath()));

    if constexpr (HostOS != OS::Windows) {
        using std::filesystem::perms;
        const perms mode = std::filesystem::status(CredentialsPath()).permissions();
        CHECK((mode & perms::group_all) == perms::none);
        CHECK((mode & perms::others_all) == perms::none);
    }

    // No temporary is left beside the real file once the write completes.
    std::filesystem::path temp = CredentialsPath();
    temp += ".tmp";
    CHECK_FALSE(std::filesystem::exists(temp));
}

TEST_CASE("the registry base falls back from the flag to the environment") {
    const ScopedUserDataDir sandbox;

    REQUIRE(UnsetEnv(kRegistryVariable));
    CHECK(ResolveRegistryBase("") == official);
    CHECK(ResolveRegistryBase("http://localhost:8080/") == local);

    REQUIRE(SetEnv(kRegistryVariable, "http://localhost:8080/"));
    CHECK(ResolveRegistryBase("") == local);
    // An explicit --registry still wins over the environment.
    CHECK(ResolveRegistryBase("https://api.rux-lang.dev/") == official);

    REQUIRE(UnsetEnv(kRegistryVariable));
    CHECK(ResolveRegistryBase("") == official);
}
