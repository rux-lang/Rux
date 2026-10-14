#include "Driver/PackageCache.h"

#include "Driver/BuildCache.h"
#include "System/Os.h"

#include <format>
#include <system_error>

namespace Rux::Driver {
std::filesystem::path PackageCacheDirectory(const std::optional<std::filesystem::path> &configured) {
    if (configured)
        return *configured;
    if (auto variable = System::GetEnvPath("RUX_PACKAGE_CACHE"); variable && !variable->empty())
        return *variable;
    return System::UserDataDir() / "Cache" / "Packages";
}

std::optional<ModulePack> ReadNewestPackageVariant(const std::filesystem::path &keyDirectory) {
    std::error_code error;
    std::optional<std::filesystem::path> newest;
    std::filesystem::file_time_type newestTime{};
    for (const auto &entry : std::filesystem::directory_iterator(keyDirectory, error)) {
        if (entry.path().extension() != ".modules")
            continue;
        std::error_code timeError;
        const auto time = entry.last_write_time(timeError);
        // The newest wins; of variants written at once, the one whose name sorts last, so every build picks the same.
        if (!timeError && (!newest || time > newestTime || (time == newestTime && entry.path() > *newest))) {
            newest = entry.path();
            newestTime = time;
        }
    }
    if (!newest)
        return std::nullopt;
    auto pack = ModulePack::Read(*newest);
    if (pack.Size() == 0)
        return std::nullopt;
    return pack;
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
