// Binary serialization of RcuFile to the on-disk RCU object format.

#include "Object/Rcu/RcuWriter.h"

#include "Object/Rcu/RcuSerialization.h"
#include "Object/Rcu/RcuStringTable.h"

#include <cstring>
#include <fstream>

namespace Rux {
bool RcuWriter::Write(const RcuFile &file, const std::filesystem::path &path) {
    const auto bytes = SerializeRcuFile(file);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

namespace {
// CRC-32C (Castagnoli)
uint32_t Crc32cTable[256];
bool Crc32cReady = false;

void InitCrc32c() {
    if (Crc32cReady) {
        return;
    }
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (0x82F6'3B78u ^ (c >> 1)) : (c >> 1);
        }
        Crc32cTable[i] = c;
    }
    Crc32cReady = true;
}

uint32_t Crc32c(const std::vector<uint8_t> &data) {
    InitCrc32c();
    uint32_t crc = 0xFFFF'FFFFu;
    for (uint8_t b : data) {
        crc = Crc32cTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFF'FFFFu;
}

// Binary writer
class BinaryWriter {
public:
    static std::vector<uint8_t> Serialize(const RcuFile &f) {
        BinaryWriter w(f);
        return w.Build();
    }

private:
    const RcuFile &f_;
    RcuStringTable st_;

    explicit BinaryWriter(const RcuFile &f)
        : f_(f) {
    }

    // Intern all strings first so offsets are stable
    void InternStrings() {
        st_.Intern(f_.sourcePath);
        st_.Intern(f_.packageName);
        for (const auto &s : f_.symbols) {
            st_.Intern(s.name);
            st_.Intern(s.typeName);
        }
        for (const auto &sec : f_.sections) {
            st_.Intern(sec.name);
        }
    }

    static void AppendU8(std::vector<uint8_t> &buf, uint8_t v) {
        buf.push_back(v);
    }

    static void AppendU16(std::vector<uint8_t> &buf, uint16_t v) {
        buf.push_back(v & 0xFF);
        buf.push_back(v >> 8);
    }

    static void AppendU32(std::vector<uint8_t> &buf, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            buf.push_back(v & 0xFF);
            v >>= 8;
        }
    }

    static void AppendI32(std::vector<uint8_t> &buf, int32_t v) {
        AppendU32(buf, static_cast<uint32_t>(v));
    }

    static void AppendU64(std::vector<uint8_t> &buf, uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            buf.push_back(v & 0xFF);
            v >>= 8;
        }
    }

    static void Patch32At(std::vector<uint8_t> &buf, uint32_t off, uint32_t v) {
        buf[off] = v & 0xFF;
        buf[off + 1] = (v >> 8) & 0xFF;
        buf[off + 2] = (v >> 16) & 0xFF;
        buf[off + 3] = v >> 24;
    }

    static void AlignTo(std::vector<uint8_t> &buf, int a) {
        while (buf.size() % a) {
            buf.push_back(0);
        }
    }

    std::vector<uint8_t> Build() {
        InternStrings();
        std::vector<uint8_t> out;
        out.reserve(1024);
        auto secCount = static_cast<uint16_t>(f_.sections.size());
        auto symCount = static_cast<uint32_t>(f_.symbols.size());
        // File Header (32 bytes)
        // [0-3]  magic
        out.push_back(0x52);
        out.push_back(0x43);
        out.push_back(0x55);
        out.push_back(0x00);
        // [4-5]  version 1.0
        AppendU16(out, 0x0100);
        // [6]    arch
        AppendU8(out, f_.arch);
        // [7]    flags
        AppendU8(out, f_.flags);
        // [8-9]  section_count
        AppendU16(out, secCount);
        // [10-11] reserved
        AppendU16(out, 0);
        // [12-15] symbol_count
        AppendU32(out, symCount);
        // [16-19] string_table_off (placeholder)
        auto stOffPatch = static_cast<uint32_t>(out.size());
        AppendU32(out, 0);
        // [20-23] string_table_size (placeholder)
        auto stSizePatch = static_cast<uint32_t>(out.size());
        AppendU32(out, 0);
        // [24-27] metadata_offset (placeholder)
        auto metaOffPatch = static_cast<uint32_t>(out.size());
        AppendU32(out, 0);
        // [28-31] checksum (placeholder)
        auto checksumPatch = static_cast<uint32_t>(out.size());
        AppendU32(out, 0);
        // Section Table (secCount × 40 bytes)
        // We need to write reloc offsets later; track patch positions.
        std::vector<uint32_t> secRelocOffPatches(secCount);
        std::vector<uint32_t> secRawOffPatches(secCount);
        for (uint16_t i = 0; i < secCount; ++i) {
            const auto &sec = f_.sections[i];
            // name[8]
            char name8[8] = {};
            for (int j = 0; j < 7 && j < static_cast<int>(sec.name.size()); ++j) {
                name8[j] = sec.name[j];
            }
            for (char c : name8) {
                AppendU8(out, static_cast<uint8_t>(c));
            }
            AppendU32(out, sec.type);
            AppendU32(out, sec.flags);
            secRawOffPatches[i] = static_cast<uint32_t>(out.size());
            AppendU32(out, 0); // raw_offset placeholder
            AppendU32(out, static_cast<uint32_t>(sec.data.size()));
            AppendU32(out, static_cast<uint32_t>(std::max(sec.data.size(), static_cast<size_t>(1))));
            // virtual_size
            AppendU16(out, sec.alignment);
            AppendU16(out, static_cast<uint16_t>(sec.relocs.size()));
            secRelocOffPatches[i] = static_cast<uint32_t>(out.size());
            AppendU32(out, 0); // reloc_offset placeholder
            AppendU32(out, 0); // reserved
        }
        // Symbol Table (symCount × 20 bytes)
        for (const auto &sym : f_.symbols) {
            AppendU32(out, st_.Intern(sym.name));
            AppendU32(out, sym.value);
            AppendU32(out, sym.size);
            AppendU16(out, sym.sectionIdx);
            AppendU8(out, sym.kind);
            AppendU8(out, sym.visibility);
            AppendU32(out, sym.typeName.empty() ? 0 : st_.Intern(sym.typeName));
        }
        // Section Data + Relocations
        for (uint16_t i = 0; i < secCount; ++i) {
            const auto &sec = f_.sections[i];
            // Align to section alignment
            AlignTo(out, sec.alignment);
            Patch32At(out, secRawOffPatches[i], static_cast<uint32_t>(out.size()));
            // Raw data
            out.insert(out.end(), sec.data.begin(), sec.data.end());
            // Relocations (4-byte aligned)
            if (!sec.relocs.empty()) {
                AlignTo(out, 4);
                Patch32At(out, secRelocOffPatches[i], static_cast<uint32_t>(out.size()));
                for (const auto &r : sec.relocs) {
                    AppendU32(out, r.sectionOffset);
                    AppendU32(out, r.symbolIndex);
                    AppendU16(out, r.type);
                    AppendU16(out, 0); // reserved
                    AppendI32(out, r.addend);
                }
            }
        }
        // String Table
        Patch32At(out, stOffPatch, static_cast<uint32_t>(out.size()));
        Patch32At(out, stSizePatch, st_.Size());
        const char *stData = st_.Data();
        for (uint32_t i = 0; i < st_.Size(); ++i) {
            out.push_back(static_cast<uint8_t>(stData[i]));
        }

        // Rux Metadata (64 bytes, 8-byte aligned)
        if (f_.hasMetadata) {
            AlignTo(out, 8);
            Patch32At(out, metaOffPatch, static_cast<uint32_t>(out.size()));
            // magic
            out.push_back(0x4D);
            out.push_back(0x45);
            out.push_back(0x54);
            out.push_back(0x41);
            AppendU32(out, 64); // block_size
            AppendU32(out,
                      st_.Intern(f_.sourcePath)); // source_path_off
            AppendU32(out,
                      st_.Intern(f_.packageName)); // package_name_off
            AppendU64(out, f_.buildTimestamp);
            AppendU32(out, f_.ruxVersion);
            AppendU32(out, f_.compilerFlags);
            for (uint8_t b : f_.sourceHash) {
                AppendU8(out, b);
            }
        }

        // CRC-32C
        uint32_t crc = Crc32c(out);
        Patch32At(out, checksumPatch, crc);

        return out;
    }
};
} // namespace

std::vector<std::uint8_t> SerializeRcuFile(const RcuFile &file) {
    return BinaryWriter::Serialize(file);
}
} // namespace Rux
