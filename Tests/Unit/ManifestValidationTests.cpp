#include "Package/ManifestValidation.h"

#include <doctest.h>
#include <string>
#include <string_view>
#include <utility>

using namespace Rux;
using namespace Rux::ManifestDetail;

namespace {
Manifest Validated(const std::string_view text) {
    auto document = ParseManifestSyntax(text);
    if (!document) {
        REQUIRE_MESSAGE(document.has_value(), "expected manifest syntax to parse: ", document.error().message);
    }
    auto result = ValidateManifestV1(std::move(*document));
    if (!result) {
        REQUIRE_MESSAGE(result.has_value(), "expected manifest schema to validate: ", result.error().message);
    }
    return std::move(*result);
}

ValidationError Rejected(const std::string_view text) {
    auto document = ParseManifestSyntax(text);
    if (!document) {
        REQUIRE_MESSAGE(document.has_value(), "expected manifest syntax to parse: ", document.error().message);
    }
    auto result = ValidateManifestV1(std::move(*document));
    REQUIRE_MESSAGE(!result.has_value(), "expected manifest schema to be rejected");
    return std::move(result.error());
}

std::string PackageWith(const std::string_view body) {
    return std::string("[Manifest]\nVersion = 1\n\n[Package]\nName = \"App\"\nVersion = \"0.1.0\"\n"
                       "Type = \"Executable\"\n") +
           std::string(body);
}

std::string StringArray(const std::string_view prefix, const std::size_t count) {
    std::string result = "[";
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            result += ", ";
        }
        result += '"' + std::string(prefix) + std::to_string(i) + '"';
    }
    return result + ']';
}
} // namespace

TEST_CASE("Manifest Version 1 validation maps the private value tree") {
    const Manifest manifest = Validated(R"([Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Rux"
Name = "App"
Version = "1.2.3"
Type = "SourceLibrary"
Authors = ["Rux Contributors"]
Keywords = ["Compiler"]
LicenseFile = "LICENSE.md"
Repository = "https://rux-lang.dev/Rux"

[Dependencies]
Core = { Namespace = "Rux", Version = "^1.0.0", TargetOS = ["Linux", "Windows"] }
Local = { Package = "Support", Path = "../Support" }

[Build]
Output = "../Artifacts"

[Build.Defines]
Channel = "Nightly"
Retries = 3
Tracing = true
)");

    CHECK(manifest.header.minRux->Text() == "0.4.0");
    CHECK(manifest.package.ns->Text() == "Rux");
    CHECK(manifest.package.type == ManifestPackageType::SourceLibrary);
    CHECK(manifest.package.licenseFile == "LICENSE.md");
    REQUIRE(manifest.dependencies.size() == 2);
    CHECK(manifest.dependencies[0].Registry()->version.Text() == "^1.0.0");
    CHECK(manifest.dependencies[0].targetOS == std::vector{Target::OS::Linux, Target::OS::Windows});
    CHECK(manifest.dependencies[1].package.Text() == "Support");
    CHECK(manifest.dependencies[1].Path() == "../Support");
    CHECK(manifest.build.output == "../Artifacts");
    CHECK(manifest.build.defines.at("Retries").kind == DefineValue::Kind::Integer);
    CHECK(manifest.build.defines.at("Tracing").text == "true");
}

TEST_CASE("Manifest Version 1 field and section names are exact") {
    SUBCASE("section casing") {
        const ValidationError error = Rejected("[manifest]\nVersion = 1\n");
        CHECK(error.location.line == 1);
        CHECK(error.location.column == 1);
        CHECK(error.message == "unknown section '[manifest]'");
    }

    SUBCASE("field casing") {
        const ValidationError error = Rejected(PackageWith("name = \"Other\"\n"));
        CHECK(error.location.line == 8);
        CHECK(error.location.column == 1);
        CHECK(error.message == "unknown field 'name' in '[Package]'");
    }
}

TEST_CASE("Manifest Version 1 requires and forbids sections by document kind") {
    SUBCASE("missing header") {
        const ValidationError error = Rejected("[Workspace]\nPackages = [\"Packages/Core\"]\n");
        CHECK(error.location.line == 1);
        CHECK(error.location.column == 1);
        CHECK(error.message == "manifest must start with a '[Manifest]' section declaring 'Version = 1'");
    }

    SUBCASE("workspace build settings") {
        const ValidationError error = Rejected(R"([Manifest]
Version = 1
[Workspace]
Packages = ["Packages/Core"]
[Build.Defines]
Mode = "Debug"
)");
        CHECK(error.location.line == 5);
        CHECK(error.message == "a workspace cannot declare build settings");
    }
}

TEST_CASE("Manifest Version 1 path diagnostics retain value locations") {
    const ValidationError error = Rejected(PackageWith("\n[Build]\nOutput = \"Artifacts\\\\Release\"\n"));
    CHECK(error.location.line == 10);
    CHECK(error.location.column == 10);
    CHECK(error.message == "'Output' must use '/' separators");
}

TEST_CASE("Manifest Version 1 enforces collection limits") {
    SUBCASE("authors") {
        const ValidationError error =
            Rejected(PackageWith("Authors = " + StringArray("Author", manifestMaxAuthors + 1) + "\n"));
        CHECK(error.message == "at most 32 authors are allowed");
    }

    SUBCASE("keywords") {
        const ValidationError error =
            Rejected(PackageWith("Keywords = " + StringArray("Keyword", manifestMaxKeywords + 1) + "\n"));
        CHECK(error.message == "at most 32 keywords are allowed");
    }

    SUBCASE("workspace packages") {
        const ValidationError error = Rejected("[Manifest]\nVersion = 1\n[Workspace]\nPackages = " +
                                               StringArray("Package", manifestMaxWorkspacePackages + 1) + "\n");
        CHECK(error.message == "at most 256 workspace packages are allowed");
    }

    SUBCASE("dependencies") {
        std::string dependencies = "\n[Dependencies]\n";
        for (std::size_t i = 0; i <= manifestMaxDependencies; ++i) {
            dependencies += "Dep" + std::to_string(i) + " = { Path = \"../Dep" + std::to_string(i) + "\" }\n";
        }
        const ValidationError error = Rejected(PackageWith(dependencies));
        CHECK(error.message == "at most 256 dependencies are allowed");
    }

    SUBCASE("defines") {
        std::string defines = "\n[Build.Defines]\n";
        for (std::size_t i = 0; i <= manifestMaxDefines; ++i) {
            defines += "Value" + std::to_string(i) + " = " + std::to_string(i) + '\n';
        }
        const ValidationError error = Rejected(PackageWith(defines));
        CHECK(error.message == "at most 128 defines are allowed");
    }
}
