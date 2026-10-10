// Module-level assembly text: sections, symbols and data definitions.

#include "CodeGen/X86_64/AssemblyModulePrinter.h"

#include "CodeGen/ConstantData.h"
#include "CodeGen/FloatLiteral.h"
#include "Ir/Lir/Lir.h"
#include "Target/Platform.h"
#include "Unicode/Utf.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <span>
#include <vector>

namespace Rux {
AssemblyModulePrinter::AssemblyModulePrinter(const Target::OS inputTargetOs)
    : targetOs(inputTargetOs) {
}

namespace {
/// Bytes as `db` lines of sixteen, the way the object's read-only data holds them.
void PrintBytes(std::ostringstream &out, const std::span<const std::uint8_t> bytes) {
    for (std::size_t start = 0; start < bytes.size(); start += 16) {
        out << "    db    ";
        for (std::size_t index = start; index < std::min(bytes.size(), start + 16); ++index) {
            out << (index == start ? "" : ", ") << std::format("0x{:02x}", bytes[index]);
        }
        out << '\n';
    }
}
} // namespace

void AssemblyModulePrinter::EmitModuleData(const LirModule &module, const Layout::LayoutMap &layouts,
                                           const std::unordered_set<std::string> &interfaceNames) {
    for (const auto &externalVariable : module.externVars) {
        DeclareExtern(externalVariable.name);
        data << "; extern var: " << externalVariable.name << '\n';
    }

    // Each constant is printed with the bytes the object emitter places in its read-only data, at the same alignment.
    for (const auto &constant : module.consts) {
        if (constant.hasSequenceData && constant.type.kind != TypeRef::Kind::Array) {
            const int elementSize = std::max(Layout::SizeOf(constant.elementType), 1);
            std::vector<std::uint8_t> elements;
            std::uint64_t length = constant.elements.size();
            if (constant.isTextSlice) {
                const std::string encoded = Layout::EncodeStringLiteral(constant.text, elementSize);
                elements.assign(encoded.begin(), encoded.end());
                elements.push_back(0);
                length = CodeUnitCount(constant.text, elementSize).value_or(0);
            }
            for (const auto &element : constant.elements) {
                AppendScalarConstant(elements, element, constant.elementType);
            }
            rodata << "align " << Layout::AlignOf(constant.elementType) << '\n';
            rodata << constant.name << "$elements:\n";
            PrintBytes(rodata, elements);
            if (constant.isPublic) {
                rodata << "global " << constant.name << '\n';
            }
            rodata << "align 8\n" << constant.name << ":  ; " << constant.type.ToString() << '\n';
            rodata << "    dq    " << constant.name << "$elements, " << length << '\n';
            continue;
        }
        if (constant.hasSequenceData || constant.hasValueData) {
            std::vector<std::uint8_t> bytes;
            int alignment = 1;
            if (constant.hasSequenceData) {
                for (const auto &element : constant.elements) {
                    AppendScalarConstant(bytes, element, constant.elementType);
                }
                alignment = Layout::AlignOf(constant.elementType);
            }
            else {
                bytes = ConstantValueBytes(constant, layouts, interfaceNames);
                alignment = ConstantValueAlignment(constant, layouts, interfaceNames);
            }
            if (constant.isPublic) {
                rodata << "global " << constant.name << '\n';
            }
            rodata << "align " << alignment << '\n';
            rodata << constant.name << ":  ; " << constant.type.ToString() << " = " << constant.value << '\n';
            PrintBytes(rodata, bytes);
            continue;
        }
        if (constant.isPublic) {
            data << "global " << constant.name << '\n';
        }
        data << constant.name << ":  ; " << constant.type.ToString() << " = " << constant.value << '\n';
        data << "    ; (constant with no laid-out value: a zeroed placeholder)\n";
    }

    // A module variable is printed where the object emitter places it: initialized data when any byte is non-zero,
    // reserved zero-fill storage otherwise.
    for (const auto &global : module.globals) {
        const std::vector<std::uint8_t> bytes = ValueBytes(global.type, global.scalars, layouts, interfaceNames);
        const bool zero = std::ranges::all_of(bytes, [](const std::uint8_t byte) { return byte == 0; });
        std::ostringstream &section = zero ? bss : data;
        if (global.isPublic) {
            section << "global " << global.name << '\n';
        }
        section << "align " << ValueAlignment(global.type, layouts, interfaceNames) << '\n';
        section << global.name << ":  ; var " << global.type.ToString() << '\n';
        if (zero) {
            section << "    resb  " << bytes.size() << '\n';
        }
        else {
            PrintBytes(section, bytes);
        }
    }

    for (const auto &vtable : module.vtables) {
        rodata << vtable.label << ":\n";
        for (const auto &method : vtable.methods) {
            rodata << "    dq " << method << '\n';
        }
    }
}

bool AssemblyModulePrinter::DeclareFunction(const LirFunc &function) {
    if (function.isExtern) {
        DeclareExtern(function.name);
        return false;
    }
    if (function.isPublic) {
        DeclareGlobal(function.name);
    }
    return true;
}

void AssemblyModulePrinter::DeclareExtern(const std::string &name) {
    if (declaredExterns.insert(name).second) {
        externs << "extern " << name << '\n';
    }
}

void AssemblyModulePrinter::DeclareGlobal(const std::string &name) {
    globals << "global " << name << '\n';
}

std::string AssemblyModulePrinter::InternString(const std::string &value) {
    if (const auto existing = stringLabels.find(value); existing != stringLabels.end()) {
        return existing->second;
    }

    std::string label = std::format("__str{}", constantIndex++);
    stringLabels[value] = label;
    rodata << label << ":\n    db    ";
    for (const unsigned char character : value) {
        rodata << static_cast<int>(character) << ", ";
    }
    rodata << "0\n";
    return label;
}

std::string AssemblyModulePrinter::InternFloat32(const std::string &value) {
    if (const auto existing = float32Labels.find(value); existing != float32Labels.end()) {
        return existing->second;
    }

    std::string label = std::format("__f32_{}", constantIndex++);
    float32Labels[value] = label;
    std::uint32_t bits;
    if (value.starts_with("0x")) {
        bits = static_cast<std::uint32_t>(std::stoull(value, nullptr, 16));
    }
    else {
        const float parsed = ParseFloatLiteral<float>(value);
        std::memcpy(&bits, &parsed, sizeof(bits));
    }
    rodata << label << ":\n    dd    0x" << std::hex << bits << std::dec << '\n';
    return label;
}

std::string AssemblyModulePrinter::InternFloat64(const std::string &value) {
    if (const auto existing = float64Labels.find(value); existing != float64Labels.end()) {
        return existing->second;
    }

    std::string label = std::format("__f64_{}", constantIndex++);
    float64Labels[value] = label;
    std::uint64_t bits;
    if (value.starts_with("0x")) {
        bits = std::stoull(value, nullptr, 16);
    }
    else {
        const double parsed = ParseFloatLiteral<double>(value);
        std::memcpy(&bits, &parsed, sizeof(bits));
    }
    rodata << label << ":\n    dq    0x" << std::hex << bits << std::dec << '\n';
    return label;
}

std::string AssemblyModulePrinter::CreateLocalLabel(const std::string_view prefix) {
    return std::format("{}{}", prefix, constantIndex++);
}

void AssemblyModulePrinter::TextLine(const std::string_view line) {
    text << line << '\n';
}

void AssemblyModulePrinter::TextInstruction(const std::string_view instruction) {
    text << "    " << instruction << '\n';
}

void AssemblyModulePrinter::TextLabel(const std::string_view label) {
    text << label << ":\n";
}

void AssemblyModulePrinter::TextComment(const std::string_view comment) {
    text << "    ; " << comment << '\n';
}

void AssemblyModulePrinter::TextBlank() {
    text << '\n';
}

bool AssemblyModulePrinter::UsesWin64Convention() const {
    return PlatformDefaultConvention(targetOs, Target::Arch::X86_64) == CallingConvention::Win64;
}

std::string AssemblyModulePrinter::Finalize() {
    std::ostringstream output;
    output << "; Generated by Rux Compiler\n";
    if (UsesWin64Convention()) {
        output << "; Target:  x86-64  (Windows x64 ABI, NASM syntax)\n";
        output << "; Calling: rcx/rdx/r8/r9 (int args), xmm0-3 (float args)\n";
    }
    else {
        output << "; Target:  x86-64  (System V AMD64 ABI, NASM syntax)\n";
        output << "; Calling: rdi/rsi/rdx/rcx/r8/r9 (int args), xmm0-7 (float args)\n";
    }
    output << "; Scratch: r10, r11 (caller-saved)\n\n";
    output << "bits 64\n\n";
    if (const std::string declarations = externs.str(); !declarations.empty()) {
        output << declarations << '\n';
    }
    if (const std::string declarations = globals.str(); !declarations.empty()) {
        output << declarations << '\n';
    }
    if (const std::string section = rodata.str(); !section.empty()) {
        output << "section .rodata\n" << section << '\n';
    }
    if (const std::string section = data.str(); !section.empty()) {
        output << "section .data\n" << section << '\n';
    }
    if (const std::string section = bss.str(); !section.empty()) {
        output << "section .bss\n" << section << '\n';
    }
    if (const std::string section = text.str(); !section.empty()) {
        output << "section .text\n" << section;
    }
    return output.str();
}
} // namespace Rux
