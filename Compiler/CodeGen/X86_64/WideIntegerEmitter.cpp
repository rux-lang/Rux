#include "CodeGen/X86_64/Encoder.h"
#include "CodeGen/X86_64/FunctionEmitter.h"

namespace Rux {
std::uint32_t X86_64FunctionEmitter::JumpIf(const std::uint8_t condition) const {
    encoder.Byte(0x0F);
    encoder.Byte(condition);
    const std::uint32_t offset = encoder.Size();
    encoder.Dword(0);
    return offset;
}

void X86_64FunctionEmitter::PatchHere(const std::uint32_t offset) const {
    encoder.Patch32(offset, static_cast<std::int32_t>(encoder.Size()) - static_cast<std::int32_t>(offset + 4));
}

void X86_64FunctionEmitter::ZeroWide(const std::int32_t destination, const int size) const {
    encoder.MovEaxImm32(0);
    for (int offset = 0; offset < size; offset += 8) {
        encoder.MovRaxStore(destination + offset);
    }
}

void X86_64FunctionEmitter::CopyWide(const std::int32_t source, const std::int32_t destination, const int size) const {
    if (source == destination) {
        return;
    }
    for (int offset = 0; offset < size; offset += 8) {
        encoder.MovRaxLoad(source + offset);
        encoder.MovRaxStore(destination + offset);
    }
}

void X86_64FunctionEmitter::NegateWide(const std::int32_t value, const int size) const {
    for (int offset = 0; offset < size; offset += 8) {
        encoder.MovEaxImm32(0); // mov preserves the borrow from the preceding word
        encoder.Byte(0x48);
        encoder.Byte(offset == 0 ? 0x2B : 0x1B); // sub/sbb rax, [rbp + disp32]
        encoder.Byte(0x85);
        encoder.Dword(static_cast<std::uint32_t>(value + offset));
        encoder.MovRaxStore(value + offset);
    }
}

void X86_64FunctionEmitter::MultiplyWide(const std::int32_t left, const std::int32_t right,
                                         const std::int32_t destination, const int size) const {
    ZeroWide(destination, size);
    const int words = size / 8;
    for (int leftWord = 0; leftWord < words; ++leftWord) {
        encoder.Byte(0x45);
        encoder.Byte(0x31);
        encoder.Byte(0xD2); // xor r10d, r10d (carry)
        for (int rightWord = 0; rightWord < words - leftWord; ++rightWord) {
            encoder.MovRaxLoad(left + leftWord * 8);
            encoder.Byte(0x48);
            encoder.Byte(0xF7);
            encoder.Byte(0xA5); // mul qword [rbp + disp32]
            encoder.Dword(static_cast<std::uint32_t>(right + rightWord * 8));
            encoder.Byte(0x48);
            encoder.Byte(0x03);
            encoder.Byte(0x85); // add rax, [rbp + disp32]
            encoder.Dword(static_cast<std::uint32_t>(destination + (leftWord + rightWord) * 8));
            encoder.Byte(0x48);
            encoder.Byte(0x83);
            encoder.Byte(0xD2);
            encoder.Byte(0x00); // adc rdx, 0
            encoder.AddRaxR10();
            encoder.Byte(0x48);
            encoder.Byte(0x83);
            encoder.Byte(0xD2);
            encoder.Byte(0x00); // adc rdx, 0
            encoder.MovRaxStore(destination + (leftWord + rightWord) * 8);
            encoder.Byte(0x49);
            encoder.Byte(0x89);
            encoder.Byte(0xD2); // mov r10, rdx
        }
    }
}
} // namespace Rux
