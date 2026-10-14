#include "Driver/ModuleCache.h"

#include "Driver/BuildCache.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace Rux::Driver {
namespace {
using Optimization::LirDeclarationReferences;
using Optimization::LirModuleReferences;
using Optimization::LirSymbolReference;

/// Collect the byte ranges of the bodies `ModuleInterface` leaves out.
void CollectPrivateBodies(const Decl &declaration, std::vector<std::pair<std::uint32_t, std::uint32_t>> &bodies) {
    const auto function = [&](const FuncDecl &decl) {
        if (decl.body && decl.typeParams.empty() && decl.body->end.offset > decl.body->location.offset)
            bodies.emplace_back(decl.body->location.offset, decl.body->end.offset);
    };
    if (const auto *decl = AstCast<const FuncDecl>(&declaration)) {
        function(*decl);
    }
    else if (const auto *extension = AstCast<const ImplDecl>(&declaration)) {
        // The methods of an extension of an instantiated type are generic when the type is: other modules instantiate
        // them, so their bodies stay.
        const auto *named = AstCast<const NamedTypeExpr>(extension->extendedType.get());
        if (named == nullptr || !named->typeArgs.empty())
            return;
        for (const auto &method : extension->methods)
            if (method)
                function(*method);
    }
    else if (const auto *nested = AstCast<const ModuleDecl>(&declaration)) {
        for (const auto &item : nested->items)
            if (item)
                CollectPrivateBodies(*item, bodies);
    }
}

void PutU8(std::vector<std::uint8_t> &out, const std::uint8_t value) {
    out.push_back(value);
}

void PutU32(std::vector<std::uint8_t> &out, const std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void PutString(std::vector<std::uint8_t> &out, const std::string_view text) {
    PutU32(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

/// Reads the fields `ModulePack::Write` wrote, failing once anything runs past the end.
class PackReader {
public:
    explicit PackReader(const std::vector<std::uint8_t> &inputBytes)
        : bytes(inputBytes) {
    }

    [[nodiscard]] bool Ok() const noexcept {
        return ok;
    }

    std::uint8_t U8() {
        if (!Need(1))
            return 0;
        return bytes[position++];
    }

    std::uint32_t U32() {
        if (!Need(4))
            return 0;
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index)
            value |= static_cast<std::uint32_t>(bytes[position++]) << (8 * index);
        return value;
    }

    std::uint64_t U64() {
        if (!Need(8))
            return 0;
        std::uint64_t value = 0;
        for (int index = 0; index < 8; ++index)
            value |= static_cast<std::uint64_t>(bytes[position++]) << (8 * index);
        return value;
    }

    std::string String() {
        const std::uint32_t length = U32();
        if (!Need(length))
            return {};
        std::string text(reinterpret_cast<const char *>(bytes.data() + position), length);
        position += length;
        return text;
    }

    std::vector<std::uint8_t> Bytes() {
        const std::uint32_t length = U32();
        if (!Need(length))
            return {};
        std::vector<std::uint8_t> result(bytes.begin() + static_cast<std::ptrdiff_t>(position),
                                         bytes.begin() + static_cast<std::ptrdiff_t>(position + length));
        position += length;
        return result;
    }

    [[nodiscard]] bool AtEnd() const noexcept {
        return position == bytes.size();
    }

private:
    bool Need(const std::size_t length) {
        if (ok && length <= bytes.size() - position)
            return true;
        ok = false;
        return false;
    }

    const std::vector<std::uint8_t> &bytes;
    std::size_t position = 0;
    bool ok = true;
};

constexpr std::string_view kPackMagic = "RUXMODS3";

void PutU64(std::vector<std::uint8_t> &out, const std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}
} // namespace

std::string ModuleInterface(const Module &module, const std::string_view source) {
    if (module.bodiesDeclareItems)
        return std::string(source);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> bodies;
    for (const auto &item : module.items)
        if (item)
            CollectPrivateBodies(*item, bodies);
    std::ranges::sort(bodies);
    std::string interface;
    interface.reserve(source.size());
    std::size_t copied = 0;
    for (const auto &[begin, end] : bodies) {
        if (begin < copied || end > source.size())
            continue;
        interface.append(source.substr(copied, begin - copied));
        interface += "{}";
        copied = end;
    }
    interface.append(source.substr(copied));
    return interface;
}

namespace {
std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return {};
    const auto end = stream.tellg();
    if (end <= 0)
        return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return {};
    return bytes;
}
} // namespace

ModulePack ModulePack::Read(const std::filesystem::path &path) {
    ModulePack pack;
    const std::vector<std::uint8_t> bytes = ReadFileBytes(path);
    if (bytes.size() < kPackMagic.size() || !std::equal(kPackMagic.begin(), kPackMagic.end(), bytes.begin()))
        return pack;
    const std::vector<std::uint8_t> body(bytes.begin() + static_cast<std::ptrdiff_t>(kPackMagic.size()), bytes.end());
    PackReader reader(body);
    const std::uint32_t count = reader.U32();
    for (std::uint32_t entry = 0; entry < count && reader.Ok(); ++entry) {
        std::string module = reader.String();
        CachedModule cached;
        cached.key = reader.String();
        const std::uint32_t rootCount = reader.U32();
        for (std::uint32_t root = 0; root < rootCount && reader.Ok(); ++root) {
            const auto kind = static_cast<Optimization::LirDeclarationKind>(reader.U8());
            cached.roots.push_back({kind, reader.String()});
        }
        cached.fails = reader.U8() != 0;
        cached.sourceSize = reader.U64();
        cached.sourceModified = static_cast<std::int64_t>(reader.U64());
        cached.sourceDigest = reader.String();
        cached.interfaceDigest = reader.String();
        for (auto &declarations : cached.references.declarations) {
            const std::uint32_t declarationCount = reader.U32();
            for (std::uint32_t index = 0; index < declarationCount && reader.Ok(); ++index) {
                LirDeclarationReferences declaration;
                declaration.name = reader.String();
                declaration.libraryRoot = reader.U8() != 0;
                const std::uint32_t referenceCount = reader.U32();
                for (std::uint32_t reference = 0; reference < referenceCount && reader.Ok(); ++reference) {
                    const auto scope = static_cast<LirSymbolReference::Scope>(reader.U8());
                    declaration.references.push_back({scope, reader.String()});
                }
                declarations.push_back(std::move(declaration));
            }
        }
        cached.object = reader.Bytes();
        if (reader.Ok())
            pack.entries.insert_or_assign(std::move(module), std::move(cached));
    }
    if (!reader.Ok() || !reader.AtEnd())
        return {};
    return pack;
}

const CachedModule *ModulePack::FindAny(const std::string &module) const {
    const auto found = entries.find(module);
    return found != entries.end() ? &found->second : nullptr;
}

const CachedModule *ModulePack::Find(const std::string &module, const std::string_view key) const {
    const auto found = entries.find(module);
    return found != entries.end() && found->second.key == key ? &found->second : nullptr;
}

void ModulePack::Put(const std::string &module, CachedModule entry) {
    entries.insert_or_assign(module, std::move(entry));
}

bool ModulePack::Write(const std::filesystem::path &path) const {
    std::vector<const std::pair<const std::string, CachedModule> *> ordered;
    ordered.reserve(entries.size());
    std::size_t size = kPackMagic.size() + 4;
    for (const auto &entry : entries) {
        ordered.push_back(&entry);
        size += entry.first.size() + entry.second.key.size() + entry.second.object.size() + 256;
    }
    std::ranges::sort(ordered, {}, [](const auto *entry) { return entry->first; });
    std::vector<std::uint8_t> out;
    out.reserve(size);
    out.insert(out.end(), kPackMagic.begin(), kPackMagic.end());
    PutU32(out, static_cast<std::uint32_t>(ordered.size()));
    for (const auto *entry : ordered) {
        const CachedModule &cached = entry->second;
        PutString(out, entry->first);
        PutString(out, cached.key);
        PutU32(out, static_cast<std::uint32_t>(cached.roots.size()));
        for (const auto &root : cached.roots) {
            PutU8(out, static_cast<std::uint8_t>(root.kind));
            PutString(out, root.name);
        }
        PutU8(out, cached.fails ? 1 : 0);
        PutU64(out, cached.sourceSize);
        PutU64(out, static_cast<std::uint64_t>(cached.sourceModified));
        PutString(out, cached.sourceDigest);
        PutString(out, cached.interfaceDigest);
        for (const auto &declarations : cached.references.declarations) {
            PutU32(out, static_cast<std::uint32_t>(declarations.size()));
            for (const auto &declaration : declarations) {
                PutString(out, declaration.name);
                PutU8(out, declaration.libraryRoot ? 1 : 0);
                PutU32(out, static_cast<std::uint32_t>(declaration.references.size()));
                for (const auto &reference : declaration.references) {
                    PutU8(out, static_cast<std::uint8_t>(reference.scope));
                    PutString(out, reference.symbol);
                }
            }
        }
        PutU32(out, static_cast<std::uint32_t>(cached.object.size()));
        out.insert(out.end(), cached.object.begin(), cached.object.end());
    }

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            return false;
        stream.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!stream.good())
            return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}
} // namespace Rux::Driver
