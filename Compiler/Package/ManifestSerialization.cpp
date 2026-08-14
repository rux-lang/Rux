#include "Package/Manifest.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <ostream>
#include <ranges>
#include <sstream>

namespace Rux {
namespace {
std::string Escape(const std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (const char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04X}", static_cast<unsigned>(c));
            }
            else {
                out.push_back(c);
            }
        }
    }
    return out;
}

std::string Quoted(const std::string_view value) {
    return std::format("\"{}\"", Escape(value));
}

void WriteStringArray(std::ostream &out, const std::string_view field, const std::vector<std::string> &values) {
    out << field << " = [";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << Quoted(values[i]);
    }
    out << "]\n";
}
} // namespace

std::string Manifest::Serialize() const {
    std::ostringstream out;
    out << "[Manifest]\n";
    out << "Version = " << header.schemaVersion << '\n';
    if (header.minRux) {
        out << "MinRux = " << Quoted(header.minRux->Text()) << '\n';
    }

    if (IsWorkspace()) {
        out << "\n[Workspace]\nPackages = [\n";
        for (const auto &member : workspace.packages) {
            out << "    " << Quoted(member) << ",\n";
        }
        out << "]\n";
        return out.str();
    }

    out << "\n[Package]\n";
    if (package.ns) {
        out << "Namespace = " << Quoted(package.ns->Text()) << '\n';
    }
    out << "Name = " << Quoted(package.name.Text()) << '\n';
    out << "Version = " << Quoted(package.version.Text()) << '\n';
    out << "Type = " << Quoted(ToString(package.type)) << '\n';
    if (!package.description.empty()) {
        out << "Description = " << Quoted(package.description) << '\n';
    }
    if (!package.authors.empty()) {
        WriteStringArray(out, "Authors", package.authors);
    }
    if (!package.keywords.empty()) {
        std::vector<std::string> spellings;
        spellings.reserve(package.keywords.size());
        for (const auto &keyword : package.keywords) {
            spellings.push_back(keyword.Text());
        }
        WriteStringArray(out, "Keywords", spellings);
    }
    if (!package.license.empty()) {
        out << "License = " << Quoted(package.license) << '\n';
    }
    if (!package.licenseFile.empty()) {
        out << "LicenseFile = " << Quoted(package.licenseFile) << '\n';
    }
    if (!package.repository.empty()) {
        out << "Repository = " << Quoted(package.repository) << '\n';
    }
    if (!package.homepage.empty()) {
        out << "Homepage = " << Quoted(package.homepage) << '\n';
    }
    if (!package.readmeFile.empty()) {
        out << "ReadmeFile = " << Quoted(package.readmeFile) << '\n';
    }

    if (!dependencies.empty()) {
        std::vector<const ManifestDependency *> ordered;
        ordered.reserve(dependencies.size());
        for (const auto &dependency : dependencies) {
            ordered.push_back(&dependency);
        }
        std::ranges::sort(ordered, {},
                          [](const ManifestDependency *dependency) { return dependency->importName.Normalized(); });

        out << "\n[Dependencies]\n";
        for (const auto *dependency : ordered) {
            out << dependency->importName.Text() << " = { ";
            const bool aliased = dependency->package != dependency->importName;
            if (const auto *registry = dependency->Registry()) {
                out << "Namespace = " << Quoted(registry->ns.Text());
                if (aliased) {
                    out << ", Package = " << Quoted(dependency->package.Text());
                }
                out << ", Version = " << Quoted(registry->version.Text());
            }
            else {
                if (aliased) {
                    out << "Package = " << Quoted(dependency->package.Text()) << ", ";
                }
                out << "Path = " << Quoted(dependency->Path());
            }
            if (!dependency->targetOS.empty()) {
                out << ", TargetOS = [";
                for (std::size_t i = 0; i < dependency->targetOS.size(); ++i) {
                    if (i != 0) {
                        out << ", ";
                    }
                    out << Quoted(ManifestTargetOSName(dependency->targetOS[i]));
                }
                out << ']';
            }
            out << " }\n";
        }
    }

    if (build.output != "Bin") {
        out << "\n[Build]\n";
        out << "Output = " << Quoted(build.output) << '\n';
    }

    if (!build.defines.empty()) {
        out << "\n[Build.Defines]\n";
        for (const auto &[name, value] : build.defines) {
            out << name << " = ";
            switch (value.kind) {
            case DefineValue::Kind::String:
                out << Quoted(value.text);
                break;
            case DefineValue::Kind::Boolean:
            case DefineValue::Kind::Integer:
                out << value.text;
                break;
            }
            out << '\n';
        }
    }
    return out.str();
}

bool Manifest::Save(const std::filesystem::path &path) const {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    const std::string text = Serialize();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return file.good();
}
} // namespace Rux
