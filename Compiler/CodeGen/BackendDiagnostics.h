#pragma once

// Shared diagnostic vocabulary for the two native back ends. Instruction
// selection and encoding remain architecture-owned; this file only keeps the
// distinction between a foreign instruction, an unimplemented instruction,
// and an unsupported LIR construct identical on both sides.

#include "Diagnostics/Diagnostics.h"
#include "Ir/Lir/Lir.h"
#include "Target/AsmInstr.h"
#include "Target/TargetTriple.h"

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace Rux {
/// Name a target the way a backend diagnostic should: its canonical `<os>-<arch>` id, which is the spelling a reader
/// could pass back to `--target`. Falls back to naming the architecture alone for a combination with no triple.
[[nodiscard]] inline std::string BackendTargetName(const Target::OS os, const Target::Arch arch) {
    if (const auto target = Target::TargetTriple::From(os, arch)) {
        return std::string(target->CanonicalName());
    }
    return std::format("unknown-{}", Target::ToString(arch));
}

/// A backend diagnostic before it becomes a `Diagnostic`. Backends build this, and the caller attaches the source
/// location, keeping instruction selection free of diagnostic plumbing.
struct AsmInstructionDiagnostic {
    std::string message;
    std::vector<std::string> notes;
    std::optional<std::string> help;
};

/// The diagnostic for an `asm func` body parsed for one architecture and then emitted for another. Distinct from an
/// unknown instruction: the assembly is valid, just not for this target.
[[nodiscard]] inline AsmInstructionDiagnostic ParsedAssemblyArchitectureDiagnostic(const std::string_view mnemonic,
                                                                                   const Target::Arch parsedArch,
                                                                                   const Target::OS targetOs,
                                                                                   const Target::Arch targetArch) {
    return {
        .message = std::format("inline assembly parsed for {} cannot be emitted for target '{}'",
                               Target::ToDisplayString(parsedArch), BackendTargetName(targetOs, targetArch)),
        .notes = {std::format("instruction: '{}'", mnemonic)},
        .help = "compile the assembly body for the architecture it was parsed for",
    };
}

/// Explain a register name the target does not have.
///
/// Checks the other architecture first, so writing `rax` in an AArch64 body is reported as an x86-64 register used for
/// the wrong target rather than as an unknown name, which is almost always what actually happened.
[[nodiscard]] inline AsmInstructionDiagnostic
ClassifyAsmRegister(const std::string_view name, const Target::OS targetOs, const Target::Arch targetArch) {
    const Target::Arch other = targetArch == Target::Arch::AArch64 ? Target::Arch::X86_64 : Target::Arch::AArch64;
    if (LookupRegister(other, name).valid) {
        return {
            .message = std::format("register '{}' is not available for target '{}'", name,
                                   BackendTargetName(targetOs, targetArch)),
            .notes = {std::format("'{}' is an {} register", name, Target::ToDisplayString(other))},
            .help = {},
        };
    }
    return {
        .message = std::format("unknown {} register '{}'", Target::ToDisplayString(targetArch), name),
        .notes = {},
        .help = {},
    };
}

/// An immediate that does not fit the signed 32-bit field the encoding provides. The range is spelled out in a note
/// because the limit is the instruction encoding's, not the type's.
[[nodiscard]] inline AsmInstructionDiagnostic Signed32EncodingRangeDiagnostic(const std::string_view mnemonic,
                                                                              const std::int64_t value) {
    return {
        .message = std::format("immediate {} is outside the signed 32-bit encoding range for '{}'", value, mnemonic),
        .notes = {"supported range: -2147483648 to 2147483647"},
        .help = {},
    };
}

/// The instruction on this target that does what the one the author wrote does elsewhere.
///
/// Someone writing `idiv` in an AArch64 body wanted signed division, not that exact mnemonic, so naming `sdiv` answers
/// the real question rather than only reporting that the name is unknown.
///
/// @return nullopt when no equivalent is known, leaving the caller's plain diagnostic unhelped
[[nodiscard]] inline std::optional<std::string> ArchitectureEquivalentHelp(const std::string_view mnemonic,
                                                                           const Target::Arch targetArch) {
    if (targetArch == Target::Arch::AArch64) {
        if (mnemonic == "imul") {
            return "use 'mul' for integer multiplication on AArch64";
        }
        if (mnemonic == "idiv") {
            return "use 'sdiv' for signed integer division on AArch64";
        }
        if (mnemonic == "syscall") {
            return "use 'svc' to request an operating-system service on AArch64";
        }
    }
    if (targetArch == Target::Arch::X86_64) {
        if (mnemonic == "sdiv") {
            return "use 'idiv' for signed integer division on x86-64";
        }
        if (mnemonic == "udiv") {
            return "use 'div' for unsigned integer division on x86-64";
        }
        if (mnemonic == "svc") {
            return "use 'syscall' to request an operating-system service on x86-64";
        }
    }
    return std::nullopt;
}

/// Explain a mnemonic the assembler did not emit, distinguishing the three reasons it can happen: the target knows the
/// instruction but has not implemented it, the instruction belongs to the other architecture, or the name is not an
/// instruction at all. Only the first is a compiler limitation rather than a mistake in the source.
[[nodiscard]] inline AsmInstructionDiagnostic
ClassifyAsmInstruction(const std::string_view mnemonic, const Target::OS targetOs, const Target::Arch targetArch) {
    const std::string target = BackendTargetName(targetOs, targetArch);
    if (IsAsmMnemonic(targetArch, mnemonic)) {
        return {
            .message = std::format("instruction '{}' is recognized for target '{}' but is not implemented by its "
                                   "assembler",
                                   mnemonic, target),
            .notes = {"this is an internal compiler limitation, not malformed inline assembly"},
            .help = "avoid this instruction until the selected backend implements it",
        };
    }

    const Target::Arch instructionArch = AsmMnemonicArch(mnemonic);
    if (instructionArch != Target::Arch::Unknown && instructionArch != targetArch) {
        return {
            .message = std::format("instruction '{}' is not available for target '{}'", mnemonic, target),
            .notes = {std::format("'{}' is an {} instruction", mnemonic, Target::ToDisplayString(instructionArch))},
            .help = ArchitectureEquivalentHelp(mnemonic, targetArch),
        };
    }

    if (const auto closest = ClosestAsmMnemonic(targetArch, mnemonic)) {
        return {.message = std::format("unknown instruction '{}'; did you mean '{}'?", mnemonic, *closest),
                .notes = {},
                .help = {}};
    }
    return {.message = std::format("unknown instruction '{}'", mnemonic), .notes = {}, .help = {}};
}

/// Report LIR the selected backend cannot lower.
///
/// This is always a compiler limitation, never a user error: the frontend already accepted the program, so the note and
/// help say so and ask for a report rather than suggesting the source be changed.
///
/// @param detail Optional specifics appended to the message, such as which operand shape was unsupported
[[nodiscard]] inline Diagnostic
UnsupportedBackendConstructDiagnostic(const std::string_view construct, const Target::OS targetOs,
                                      const Target::Arch targetArch, const std::string_view functionName,
                                      const std::optional<std::string_view> detail = {}) {
    std::string message =
        std::format("cannot generate {} for target '{}'", construct, BackendTargetName(targetOs, targetArch));
    if (!functionName.empty()) {
        message += std::format(" in function '{}'", functionName);
    }
    if (detail && !detail->empty()) {
        message += std::format(": {}", *detail);
    }
    return ErrorDiagnostic(std::move(message),
                           {"the frontend accepted this program, but the selected backend cannot lower this LIR"},
                           "report this internal compiler limitation with the target and source program");
}

/// The same report named by opcode, which is how a backend's instruction selector reaches it. An opcode outside the
/// known set is reported by number rather than dropped, since that case means the IR itself is corrupt.
[[nodiscard]] inline Diagnostic UnsupportedLirDiagnostic(const LirOpcode opcode, const Target::OS targetOs,
                                                         const Target::Arch targetArch,
                                                         const std::string_view functionName,
                                                         const std::optional<std::string_view> detail = {}) {
    const std::string_view name = LirOpcodeName(opcode);
    const std::string construct = name == "?" ? std::format("unknown LIR opcode {}", static_cast<std::uint32_t>(opcode))
                                              : std::format("LIR instruction '{}'", name);
    return UnsupportedBackendConstructDiagnostic(construct, targetOs, targetArch, functionName, detail);
}
} // namespace Rux
