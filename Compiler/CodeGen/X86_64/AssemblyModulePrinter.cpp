// Module-level assembly text: sections, symbols and data definitions.

#include "CodeGen/X86_64/AssemblyModulePrinter.h"

#include "CodeGen/FloatLiteral.h"
#include "CodeGen/X86_64/RuntimeHelpers.h"
#include "Ir/Lir/Lir.h"
#include "Target/Platform.h"

#include <cstring>
#include <format>

namespace Rux {
AssemblyModulePrinter::AssemblyModulePrinter(const Target::OS inputTargetOs)
    : targetOs(inputTargetOs) {
}

void AssemblyModulePrinter::EmitModuleData(const LirModule &module) {
    for (const auto &externalVariable : module.externVars) {
        DeclareExtern(externalVariable.name);
        data << "; extern var: " << externalVariable.name << '\n';
    }

    for (const auto &constant : module.consts) {
        if (constant.isPublic) {
            data << "global " << constant.name << '\n';
        }
        data << constant.name << ":  ; " << constant.type.ToString() << " = " << constant.value << '\n';
        data << "    ; (constant — initialized at link time)\n";
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

void AssemblyModulePrinter::RequestHelper(const X86_64RuntimeHelper helper) {
    switch (helper) {
    case X86_64RuntimeHelper::IntegerPower:
        needsIntegerPower = true;
        break;
    case X86_64RuntimeHelper::FloatPower64:
        needsFloatPower64 = true;
        break;
    case X86_64RuntimeHelper::FloatPower32:
        needsFloatPower64 = true;
        needsFloatPower32 = true;
        break;
    }
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

void AssemblyModulePrinter::EmitRequestedHelpers() {
    if (needsIntegerPower) {
        EmitIntegerPowerHelper();
    }
    if (needsFloatPower64) {
        EmitFloatPower64Helper();
    }
    if (needsFloatPower32) {
        EmitFloatPower32Helper();
    }
}

void AssemblyModulePrinter::EmitIntegerPowerHelper() {
    TextBlank();
    TextLabel("__rux_ipow");
    TextInstruction("test    rdx, rdx");
    TextInstruction("js      .negative");
    TextInstruction("mov     eax, 1");
    TextLabel(".loop");
    TextInstruction("test    rdx, rdx");
    TextInstruction("jz      .done");
    TextInstruction("test    rdx, 1");
    TextInstruction("jz      .square");
    TextInstruction("imul    rax, rcx");
    TextLabel(".square");
    TextInstruction("imul    rcx, rcx");
    TextInstruction("sar     rdx, 1");
    TextInstruction("jmp     .loop");
    TextLabel(".negative");
    TextInstruction("xor     eax, eax");
    TextLabel(".done");
    TextInstruction("ret");
}

void AssemblyModulePrinter::EmitFloatPower64Helper() {
    TextBlank();
    TextLabel("__rux_powf64");
    TextInstruction("sub     rsp, 16");
    TextInstruction("movsd   [rsp], xmm0");
    TextInstruction("movsd   [rsp + 8], xmm1");
    TextInstruction("mov     rax, [rsp + 8]");
    TextInstruction("add     rax, rax");
    TextInstruction("jnz     .not_exp0");
    TextInstruction("mov     rax, 0x3FF0000000000000");
    TextInstruction("mov     [rsp], rax");
    TextInstruction("movsd   xmm0, [rsp]");
    TextInstruction("add     rsp, 16");
    TextInstruction("ret");
    TextLabel(".not_exp0");
    TextInstruction("mov     rax, [rsp]");
    TextInstruction("add     rax, rax");
    TextInstruction("jnz     .base_nonzero");
    TextInstruction("mov     rax, [rsp + 8]");
    TextInstruction("test    rax, rax");
    TextInstruction("js      .base0_neg");
    TextInstruction("xor     eax, eax");
    TextInstruction("mov     [rsp], rax");
    TextInstruction("movsd   xmm0, [rsp]");
    TextInstruction("add     rsp, 16");
    TextInstruction("ret");
    TextLabel(".base0_neg");
    TextInstruction("mov     rax, 0x7FF0000000000000");
    TextInstruction("mov     [rsp], rax");
    TextInstruction("movsd   xmm0, [rsp]");
    TextInstruction("add     rsp, 16");
    TextInstruction("ret");
    TextLabel(".base_nonzero");
    TextInstruction("xor     edx, edx");
    TextInstruction("mov     rax, [rsp]");
    TextInstruction("test    rax, rax");
    TextInstruction("jns     .magnitude");
    TextInstruction("movsd   xmm2, [rsp + 8]");
    TextInstruction("cvttsd2si rax, xmm2");
    TextInstruction("cvtsi2sd xmm3, rax");
    TextInstruction("ucomisd xmm2, xmm3");
    TextInstruction("jne     .nonint");
    TextInstruction("and     eax, 1");
    TextInstruction("mov     edx, eax");
    TextInstruction("jmp     .magnitude");
    TextLabel(".nonint");
    TextInstruction("mov     edx, 2");
    TextLabel(".magnitude");
    TextInstruction("fld     qword [rsp + 8]");
    TextInstruction("fld     qword [rsp]");
    TextInstruction("fabs");
    TextInstruction("fyl2x");
    TextInstruction("fld     st0");
    TextInstruction("frndint");
    TextInstruction("fsub    st1, st0");
    TextInstruction("fxch");
    TextInstruction("f2xm1");
    TextInstruction("fld1");
    TextInstruction("faddp   st1, st0");
    TextInstruction("fscale");
    TextInstruction("fstp    st1");
    TextInstruction("test    edx, edx");
    TextInstruction("jz      .store");
    TextInstruction("cmp     edx, 2");
    TextInstruction("jz      .nan");
    TextInstruction("fchs");
    TextLabel(".store");
    TextInstruction("fstp    qword [rsp]");
    TextInstruction("movsd   xmm0, [rsp]");
    TextInstruction("add     rsp, 16");
    TextInstruction("ret");
    TextLabel(".nan");
    TextInstruction("fstp    st0");
    TextInstruction("mov     rax, 0x7FF8000000000000");
    TextInstruction("mov     [rsp], rax");
    TextInstruction("movsd   xmm0, [rsp]");
    TextInstruction("add     rsp, 16");
    TextInstruction("ret");
}

void AssemblyModulePrinter::EmitFloatPower32Helper() {
    TextBlank();
    TextLabel("__rux_powf32");
    TextInstruction("cvtss2sd xmm0, xmm0");
    TextInstruction("cvtss2sd xmm1, xmm1");
    TextInstruction("sub     rsp, 8");
    TextInstruction("call    __rux_powf64");
    TextInstruction("add     rsp, 8");
    TextInstruction("cvtsd2ss xmm0, xmm0");
    TextInstruction("ret");
}

std::string AssemblyModulePrinter::Finalize() {
    EmitRequestedHelpers();

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
    if (const std::string section = text.str(); !section.empty()) {
        output << "section .text\n" << section;
    }
    return output.str();
}
} // namespace Rux
