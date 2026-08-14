#include "Linker/ArchiveWriter.h"

#include "Linker/Coff/CoffObjectWriter.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace Rux {
namespace {
void PutBig32(std::vector<std::uint8_t> &out, const std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24U));
    out.push_back(static_cast<std::uint8_t>(value >> 16U));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value));
}

void PutLittle16(std::vector<std::uint8_t> &out, const std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void PutLittle32(std::vector<std::uint8_t> &out, const std::uint32_t value) {
    PutLittle16(out, static_cast<std::uint16_t>(value));
    PutLittle16(out, static_cast<std::uint16_t>(value >> 16U));
}

std::string DecimalField(const std::uint64_t value, const std::size_t width) {
    std::string result(width, ' ');
    const std::string digits = std::to_string(value);
    std::copy(digits.begin(), digits.end(), result.begin());
    return result;
}

void AppendHeader(std::vector<std::uint8_t> &out, std::string name, const std::size_t size) {
    if (name.size() <= 15 && !name.ends_with('/')) {
        name.push_back('/');
    }
    name.resize(16, ' ');
    const std::string header = name + DecimalField(0, 12) + DecimalField(0, 6) + DecimalField(0, 6) +
                               std::string("100644  ") + DecimalField(size, 10) + "`\n";
    out.insert(out.end(), header.begin(), header.end());
}

std::size_t PaddedMemberSize(const std::size_t dataSize) {
    return 60 + dataSize + (dataSize & 1U);
}

struct SymbolReference {
    std::string name;
    std::size_t memberIndex = 0;
};

std::vector<SymbolReference> CollectSymbols(const std::span<const NativeObject> objects) {
    std::vector<SymbolReference> symbols;
    for (std::size_t memberIndex = 0; memberIndex < objects.size(); ++memberIndex) {
        for (const auto &symbol : objects[memberIndex].publicSymbols) {
            symbols.push_back({symbol, memberIndex});
        }
    }
    std::ranges::sort(symbols, {}, &SymbolReference::name);
    return symbols;
}

std::vector<std::uint8_t> FirstLinkerMember(const std::vector<SymbolReference> &symbols,
                                            const std::vector<std::uint32_t> &memberOffsets) {
    std::vector<std::uint8_t> data;
    PutBig32(data, static_cast<std::uint32_t>(symbols.size()));
    for (const auto &symbol : symbols) {
        PutBig32(data, memberOffsets[symbol.memberIndex]);
    }
    for (const auto &symbol : symbols) {
        data.insert(data.end(), symbol.name.begin(), symbol.name.end());
        data.push_back(0);
    }
    return data;
}

std::vector<std::uint8_t> SecondLinkerMember(const std::vector<SymbolReference> &symbols,
                                             const std::vector<std::uint32_t> &memberOffsets) {
    std::vector<std::uint8_t> data;
    PutLittle32(data, static_cast<std::uint32_t>(memberOffsets.size()));
    for (const auto offset : memberOffsets) {
        PutLittle32(data, offset);
    }
    PutLittle32(data, static_cast<std::uint32_t>(symbols.size()));
    for (const auto &symbol : symbols) {
        PutLittle16(data, static_cast<std::uint16_t>(symbol.memberIndex + 1));
    }
    for (const auto &symbol : symbols) {
        data.insert(data.end(), symbol.name.begin(), symbol.name.end());
        data.push_back(0);
    }
    return data;
}

std::vector<std::uint8_t> BsdLinkerMember(const std::vector<SymbolReference> &symbols,
                                          const std::vector<std::uint32_t> &memberOffsets) {
    std::vector<std::uint8_t> strings;
    std::vector<std::uint32_t> stringOffsets;
    stringOffsets.reserve(symbols.size());
    for (const auto &symbol : symbols) {
        stringOffsets.push_back(static_cast<std::uint32_t>(strings.size()));
        strings.insert(strings.end(), symbol.name.begin(), symbol.name.end());
        strings.push_back(0);
    }

    std::vector<std::uint8_t> data;
    PutLittle32(data, static_cast<std::uint32_t>(symbols.size() * 8));
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        PutLittle32(data, stringOffsets[i]);
        PutLittle32(data, memberOffsets[symbols[i].memberIndex]);
    }
    PutLittle32(data, static_cast<std::uint32_t>(strings.size()));
    data.insert(data.end(), strings.begin(), strings.end());
    return data;
}

bool WriteBytes(const std::filesystem::path &path, const std::span<const std::uint8_t> bytes, std::string &error) {
    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) {
        error = "could not create archive output directory: " + filesystemError.message();
        return false;
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "could not open archive output '" + path.string() + "'";
        return false;
    }
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream.good()) {
        error = "could not write archive output '" + path.string() + "'";
        return false;
    }
    return true;
}

enum class ArchiveFormat : std::uint8_t {
    Gnu,
    Bsd,
    Coff
};

bool BuildArchive(const std::span<const NativeObject> objects, const ArchiveFormat format,
                  std::vector<std::uint8_t> &archive, std::string &error) {
    const auto symbols = CollectSymbols(objects);
    if (format == ArchiveFormat::Coff && objects.size() > std::numeric_limits<std::uint16_t>::max()) {
        error = "too many members for a COFF library";
        return false;
    }

    std::vector<std::uint32_t> zeroOffsets(objects.size());
    const auto provisionalFirst =
        format == ArchiveFormat::Bsd ? BsdLinkerMember(symbols, zeroOffsets) : FirstLinkerMember(symbols, zeroOffsets);
    const auto provisionalSecond =
        format == ArchiveFormat::Coff ? SecondLinkerMember(symbols, zeroOffsets) : std::vector<std::uint8_t>{};
    std::size_t cursor = 8 + PaddedMemberSize(provisionalFirst.size());
    if (format == ArchiveFormat::Coff) {
        cursor += PaddedMemberSize(provisionalSecond.size());
    }
    std::vector<std::uint32_t> memberOffsets;
    memberOffsets.reserve(objects.size());
    for (const auto &object : objects) {
        memberOffsets.push_back(static_cast<std::uint32_t>(cursor));
        cursor += PaddedMemberSize(object.bytes.size());
    }
    const auto first = format == ArchiveFormat::Bsd ? BsdLinkerMember(symbols, memberOffsets)
                                                    : FirstLinkerMember(symbols, memberOffsets);
    const auto second =
        format == ArchiveFormat::Coff ? SecondLinkerMember(symbols, memberOffsets) : std::vector<std::uint8_t>{};

    archive.assign({'!', '<', 'a', 'r', 'c', 'h', '>', '\n'});
    const auto appendMember = [&](const std::string &name, const std::span<const std::uint8_t> data) {
        AppendHeader(archive, name, data.size());
        archive.insert(archive.end(), data.begin(), data.end());
        if ((data.size() & 1U) != 0) {
            archive.push_back('\n');
        }
    };
    appendMember(format == ArchiveFormat::Bsd ? "__.SYMDEF SORTED" : "/", first);
    if (format == ArchiveFormat::Coff) {
        appendMember("/", second);
    }
    for (const auto &object : objects) {
        appendMember(object.name, object.bytes);
    }
    return true;
}
} // namespace

bool WriteNativeArchive(const std::span<const NativeObject> objects, const Target::OS targetOs,
                        const Target::Arch targetArch, const std::filesystem::path &outputPath, std::string &error) {
    if (RcuArchFor(targetArch) == RcuArch::Unknown) {
        error = std::format("cannot write a static library for {}: no object encoding exists for this architecture",
                            Target::ToDisplayString(targetArch));
        return false;
    }
    std::vector<std::uint8_t> archive;
    const auto format = targetOs == Target::OS::Windows ? ArchiveFormat::Coff
                      : targetOs == Target::OS::MacOS   ? ArchiveFormat::Bsd
                                                        : ArchiveFormat::Gnu;
    if (!BuildArchive(objects, format, archive, error)) {
        return false;
    }
    return WriteBytes(outputPath, archive, error);
}

bool WriteWindowsImportLibrary(const std::string_view libraryName, const std::span<const std::string> exports,
                               const Target::Arch targetArch, const std::filesystem::path &outputPath,
                               std::string &error) {
    const std::uint16_t machine = CoffMachine(targetArch);
    if (machine == 0) {
        error = std::format("cannot write an import library for {}: no COFF machine identifier exists for this "
                            "architecture",
                            Target::ToDisplayString(targetArch));
        return false;
    }
    std::vector<NativeObject> members;
    members.reserve(exports.size());
    for (const auto &symbol : exports) {
        NativeObject member;
        member.name = symbol + ".obj";
        member.publicSymbols.push_back(symbol);
        member.publicSymbols.push_back("__imp_" + symbol);
        PutLittle16(member.bytes, 0);
        PutLittle16(member.bytes, 0xffff);
        PutLittle16(member.bytes, 0);
        PutLittle16(member.bytes, machine);
        PutLittle32(member.bytes, 0);
        const auto dataSize = static_cast<std::uint32_t>(symbol.size() + 1 + libraryName.size() + 1);
        PutLittle32(member.bytes, dataSize);
        PutLittle16(member.bytes, 0);
        PutLittle16(member.bytes, 0x0004);
        member.bytes.insert(member.bytes.end(), symbol.begin(), symbol.end());
        member.bytes.push_back(0);
        member.bytes.insert(member.bytes.end(), libraryName.begin(), libraryName.end());
        member.bytes.push_back(0);
        members.push_back(std::move(member));
    }
    std::vector<std::uint8_t> archive;
    if (!BuildArchive(members, ArchiveFormat::Coff, archive, error)) {
        return false;
    }
    return WriteBytes(outputPath, archive, error);
}
} // namespace Rux
