#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace Rux {
// The first five are nonvolatile under both supported x86-64 ABIs. RSI/RDI may only be allocated when the frame
// planner proves Win64 preservation throughout the function and its calls. All emitter consumers share this mapping.
inline constexpr std::array<int, 7> X86AllocatedGprs = {3, 12, 13, 14, 15, 6, 7};

[[nodiscard]] inline std::string_view X86GeneralRegisterName(const int number) {
    constexpr std::array<std::string_view, 16> names = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                                        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    return names.at(static_cast<std::size_t>(number));
}
} // namespace Rux
