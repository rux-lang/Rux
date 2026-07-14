// Human-readable text dump of RcuFile contents.

#include "Object/Rcu/RcuDumper.h"
#include "Object/Rcu/RcuSerialization.h"

#include <format>
#include <fstream>
#include <sstream>

namespace Rux {
bool RcuDumper::Dump(const RcuFile &file, const std::filesystem::path &path) {
    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream << DumpRcuFileText(file);
    return stream.good();
}

namespace {
// Text dumper
class TextDumper {
public:
    static std::string Dump(const RcuFile &f) {
        std::ostringstream out;
        out << "; RCU  Rux Compiled Unit  v1.0\n";
        out << "; Architecture: x86-64 (Windows x64)\n";
        out << std::format("; Source:        {}\n", f.sourcePath.empty() ? "<unknown>" : f.sourcePath);
        out << std::format("; Package:       {}\n", f.packageName.empty() ? "<unknown>" : f.packageName);
        if (f.ruxVersion) {
            out << std::format("; Rux version:   {}.{}.{}\n", f.ruxVersion >> 16, (f.ruxVersion >> 8) & 0xFF,
                               f.ruxVersion & 0xFF);
        }
        out << '\n';

        // Sections
        out << std::format("Sections: {}\n", f.sections.size());
        for (size_t i = 0; i < f.sections.size(); ++i) {
            const auto &s = f.sections[i];
            std::string flags;
            if (s.flags & RcuSecFlag::Alloc) {
                flags += 'A';
            }
            if (s.flags & RcuSecFlag::Exec) {
                flags += 'E';
            }
            if (s.flags & RcuSecFlag::Read) {
                flags += 'R';
            }
            if (s.flags & RcuSecFlag::Write) {
                flags += 'W';
            }
            if (s.flags & RcuSecFlag::Merge) {
                flags += 'M';
            }
            if (s.flags & RcuSecFlag::Strings) {
                flags += 'S';
            }
            if (flags.empty()) {
                flags = "-";
            }
            out << std::format("  [{:2}]  {:<10}  flags:{:<5}  "
                               "align:{:<4}  data:{}B  relocs:{}\n",
                               i, s.name, flags, s.alignment, s.data.size(), s.relocs.size());
        }
        out << '\n';

        // Symbols
        out << std::format("Symbols: {}\n", f.symbols.size());
        for (size_t i = 0; i < f.symbols.size(); ++i) {
            const auto &s = f.symbols[i];
            std::string secStr;
            if (s.sectionIdx == RCU_SEC_EXTERNAL) {
                secStr = "extern";
            }
            else if (s.sectionIdx == RCU_SEC_ABSOLUTE) {
                secStr = "abs";
            }
            else if (s.sectionIdx < f.sections.size()) {
                secStr = std::format("{}+0x{:04X}", f.sections[s.sectionIdx].name, s.value);
            }
            else {
                secStr = std::format("sec{}+0x{:04X}", s.sectionIdx, s.value);
            }

            auto kindStr = "?";
            switch (s.kind) {
            case RcuSymKind::Func:
                kindStr = "FUNC";
                break;
            case RcuSymKind::Data:
                kindStr = "DATA";
                break;
            case RcuSymKind::Const:
                kindStr = "CONST";
                break;
            case RcuSymKind::Section:
                kindStr = "SECTION";
                break;
            case RcuSymKind::File:
                kindStr = "FILE";
                break;
            case RcuSymKind::ExternFunc:
                kindStr = "EXTFUNC";
                break;
            case RcuSymKind::ExternData:
                kindStr = "EXTDATA";
                break;
            default:;
            }
            const char *visStr = s.visibility == RcuSymVis::Global ? "GLOBAL"
                               : s.visibility == RcuSymVis::Weak   ? "WEAK"
                                                                   : "LOCAL";

            out << std::format("  [{:3}]  {:<24}  {:>20}  size={:<6}  {:<8}  {:<6}", i, s.name, secStr, s.size, kindStr,
                               visStr);
            if (!s.typeName.empty()) {
                out << std::format("  \"{}\"", s.typeName);
            }
            out << '\n';
        }
        out << '\n';

        // Relocations
        bool anyReloc = false;
        for (const auto &sec : f.sections) {
            if (!sec.relocs.empty()) {
                anyReloc = true;
                break;
            }
        }

        if (anyReloc) {
            for (size_t si = 0; si < f.sections.size(); ++si) {
                const auto &sec = f.sections[si];
                if (sec.relocs.empty()) {
                    continue;
                }
                out << std::format("Relocations ({}):\n", sec.name);
                for (size_t i = 0; i < sec.relocs.size(); ++i) {
                    const auto &r = sec.relocs[i];
                    const char *rt = "?";
                    if (r.type == RcuRelType::Abs64) {
                        rt = "ABS_64";
                    }
                    else if (r.type == RcuRelType::Abs32) {
                        rt = "ABS_32";
                    }
                    else if (r.type == RcuRelType::Rel32) {
                        rt = "REL_32";
                    }
                    std::string symName = r.symbolIndex < f.symbols.size() ? f.symbols[r.symbolIndex].name : "?";
                    out << std::format("  [{:3}]  off=0x{:04X}  "
                                       "sym[{}]={}  {}  addend={}\n",
                                       i, r.sectionOffset, r.symbolIndex, symName, rt, r.addend);
                }
                out << '\n';
            }
        }

        // Hex dumps
        for (const auto &sec : f.sections) {
            if (sec.data.empty()) {
                continue;
            }
            out << std::format("{} ({} bytes):\n", sec.name, sec.data.size());
            for (size_t i = 0; i < sec.data.size(); i += 16) {
                out << std::format("  {:04X}  ", i);
                for (size_t j = 0; j < 16; ++j) {
                    if (i + j < sec.data.size()) {
                        out << std::format("{:02X} ", sec.data[i + j]);
                    }
                    else {
                        out << "   ";
                    }
                    if (j == 7) {
                        out << ' ';
                    }
                }
                out << " |";
                for (size_t j = 0; j < 16 && i + j < sec.data.size(); ++j) {
                    unsigned char c = sec.data[i + j];
                    out << (c >= 32 && c < 127 ? static_cast<char>(c) : '.');
                }
                out << "|\n";
            }
            out << '\n';
        }

        return out.str();
    }
};
} // namespace

std::string DumpRcuFileText(const RcuFile &file) {
    return TextDumper::Dump(file);
}
} // namespace Rux
