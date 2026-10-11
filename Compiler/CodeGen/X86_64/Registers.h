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
} // namespace Rux
