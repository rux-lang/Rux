#include "Driver/PackageCache.h"

#include "Driver/BuildCache.h"
#include "System/Os.h"

#include <algorithm>
#include <format>
#include <functional>
#include <system_error>
#include <utility>
#include <vector>

namespace Rux::Driver {
std::filesystem::path PackageCacheDirectory(const std::optional<std::filesystem::path> &configured) {
    if (configured)
        return *configured;
    if (auto variable = System::GetEnvPath("RUX_PACKAGE_CACHE"); variable && !variable->empty())
        return *variable;
    return System::UserDataDir() / "Cache" / "Packages";
}

namespace {
/// The variants kept under `keyDirectory`, newest first; of variants written at once, the one whose name sorts last
/// first, so every build looks at them in the same order.
std::vector<std::filesystem::path> VariantsNewestFirst(const std::filesystem::path &keyDirectory) {
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> variants;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(keyDirectory, error)) {
        if (entry.path().extension() != ".modules")
            continue;
        std::error_code timeError;
        const auto time = entry.last_write_time(timeError);
        if (!timeError)
            variants.emplace_back(time, entry.path());
    }
    std::ranges::sort(variants, std::ranges::greater{});
    std::vector<std::filesystem::path> paths;
    paths.reserve(variants.size());
    for (auto &variant : variants)
        paths.push_back(std::move(variant.second));
    return paths;
}
} // namespace

std::optional<ModulePack> ReadNewestPackageVariant(const std::filesystem::path &keyDirectory) {
    const auto variants = VariantsNewestFirst(keyDirectory);
    if (variants.empty())
        return std::nullopt;
    auto pack = ModulePack::Read(variants.front());
    if (pack.Size() == 0)
        return std::nullopt;
    return pack;
}

std::optional<ModulePack> ReadPackageVariantWhere(const std::filesystem::path &keyDirectory,
                                                  const std::function<bool(const ModulePack &)> &accepts) {
    for (const auto &path : VariantsNewestFirst(keyDirectory)) {
        auto pack = ModulePack::Read(path);
        if (pack.Size() != 0 && accepts(pack))
            return pack;
    }
    return std::nullopt;
}

std::optional<ModulePack> ReadPackageVariantNamed(const std::filesystem::path &keyDirectory, const std::string &names,
                                                  const std::span<const Module *const> modules) {
    return ReadPackageVariantWhere(keyDirectory, [&](const ModulePack &pack) {
        return std::ranges::all_of(modules, [&](const Module *module) {
            const CachedModule *entry = pack.FindAny(module->name);
            return entry != nullptr && entry->key == names;
        });
    });
}

void WritePackageVariant(const std::filesystem::path &keyDirectory, const std::string &variant,
                         const ModulePack &pack) {
    const auto path = keyDirectory / (variant + ".modules");
    std::error_code error;
    if (std::filesystem::exists(path, error))
        return;
    static_cast<void>(pack.Write(path));
}

namespace {
void AppendNames(const SemanticModel &model, const Decl &declaration, std::string &names) {
    const auto append = [&](const Decl &named) {
        if (const auto *identity = model.TryGetSymbolIdentity(named))
            names += identity->linkerName + "\n";
    };
    if (const auto *nested = AstCast<const ModuleDecl>(&declaration)) {
        for (const auto &item : nested->items)
            if (item)
                AppendNames(model, *item, names);
    }
    else if (const auto *extension = AstCast<const ImplDecl>(&declaration)) {
        for (const auto &method : extension->methods)
            if (method)
                append(*method);
        if (const auto *vtable = model.TryGetVtableIdentity(*extension)) {
            names += vtable->linkerName + "\n";
            for (const auto &entry : vtable->entries)
                names += entry + "\n";
        }
    }
    else if (AstCast<const FuncDecl>(&declaration) || AstCast<const StructDecl>(&declaration) ||
             AstCast<const EnumDecl>(&declaration) || AstCast<const UnionDecl>(&declaration) ||
             AstCast<const InterfaceDecl>(&declaration) || AstCast<const ExternFuncDecl>(&declaration)) {
        append(declaration);
    }
}
} // namespace

std::string NamesDigest(const SemanticModel &model, const std::span<const Module *const> modules) {
    std::string names;
    for (const Module *module : modules) {
        names += "module " + module->name + "\n";
        for (const auto &item : module->items)
            if (item)
                AppendNames(model, *item, names);
    }
    return HashText(names);
}
} // namespace Rux::Driver
