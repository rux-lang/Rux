#pragma once

#include "Target/Target.h"

#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Rux {
struct LirFunc;
struct LirModule;

/// Owns package-level x86-64 assembly text: declarations, sections, interned literals, and synthesized helper bodies.
/// Function printing writes its text through this boundary so final section ordering has a single owner.
class AssemblyModulePrinter {
public:
    explicit AssemblyModulePrinter(Target::OS targetOs);

    void EmitModuleData(const LirModule &module);
    [[nodiscard]] bool DeclareFunction(const LirFunc &function);
    void DeclareExtern(const std::string &name);

    [[nodiscard]] std::string InternString(const std::string &value);
    [[nodiscard]] std::string InternFloat32(const std::string &value);
    [[nodiscard]] std::string InternFloat64(const std::string &value);
    [[nodiscard]] std::string CreateLocalLabel(std::string_view prefix);
    void TextLine(std::string_view line);
    void TextInstruction(std::string_view instruction);
    void TextLabel(std::string_view label);
    void TextComment(std::string_view comment);
    void TextBlank();

    /// Completes requested helper bodies and assembles the final NASM text.
    [[nodiscard]] std::string Finalize();

private:
    Target::OS targetOs;
    std::ostringstream text;
    std::ostringstream data;
    std::ostringstream rodata;
    std::ostringstream externs;
    std::ostringstream globals;
    std::unordered_map<std::string, std::string> stringLabels;
    std::unordered_map<std::string, std::string> float32Labels;
    std::unordered_map<std::string, std::string> float64Labels;
    std::unordered_set<std::string> declaredExterns;
    int constantIndex = 0;
    [[nodiscard]] bool UsesWin64Convention() const;
    void DeclareGlobal(const std::string &name);
};
} // namespace Rux
