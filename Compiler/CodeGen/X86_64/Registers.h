#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace Rux {
// The first five are nonvolatile under both supported x86-64 ABIs. RSI/RDI may only be allocated when the frame
// planner proves Win64 preservation throughout the function and its calls. All emitter consumers share this mapping.
inline constexpr std::array<int, 7> X86AllocatedGprs = {3, 12, 13, 14, 15, 6, 7};

[[nodiscard]] inline std::string_view X86GeneralRegisterName(const int number, const bool word32 = false) {
    constexpr std::array<std::string_view, 16> names = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                                        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    constexpr std::array<std::string_view, 16> names32 = {"eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
                                                          "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
    return (word32 ? names32 : names).at(static_cast<std::size_t>(number));
}

[[nodiscard]] inline std::string_view X86SizedRegisterName(const int number, const int bytes) {
    constexpr std::array<std::string_view, 16> byteNames = {"al",   "cl",   "dl",   "bl",  "spl",  "bpl",
                                                            "sil",  "dil",  "r8b",  "r9b", "r10b", "r11b",
                                                            "r12b", "r13b", "r14b", "r15b"};
    constexpr std::array<std::string_view, 16> wordNames = {
        "ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
    if (bytes == 1)
        return byteNames.at(static_cast<std::size_t>(number));
    if (bytes == 2)
        return wordNames.at(static_cast<std::size_t>(number));
    return X86GeneralRegisterName(number, bytes == 4);
}
} // namespace Rux
