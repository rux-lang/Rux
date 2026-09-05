#include "CodeGen/X86_64/ModuleEmitter.h"

namespace Rux::X86_64Detail {

// Load A (rax / xmm0) and B (r10 / xmm1)
void X86_64ModuleEmitter::LoadA(const LirReg reg, const TypeRef &t) const {
    const auto &physicalRegisters = FramePlan().PhysicalRegisters();
    auto it = physicalRegisters.find(reg);
    if (it != physicalRegisters.end()) {
        enc.MovRaxPhysReg(it->second);
        int sz = SizeOfRuntime(t);
        if (sz > 0 && sz < 8 && !IsAggregate(t)) {
            if (t.IsSigned()) {
                if (sz == 4)
                    enc.MovsxdRaxEax();
                else if (sz == 2)
                    enc.MovsxRaxAx();
                else
                    enc.MovsxRaxAl();
            }
            else {
                if (sz == 4)
                    enc.MovEaxEax();
                else if (sz == 2)
                    enc.MovzxRaxAx();
                else
                    enc.MovzxRaxAl();
            }
        }
        return;
    }
    const int sz = SizeOfRuntime(t);
    const int runtimeSz = SizeOfRuntime(t);
    const int32_t d = Disp(reg);
    if (IsAggregate(t) && sz > 0 && sz < 8 && sz != 1 && sz != 2 && sz != 4) {
        // A 3-, 5-, 6- or 7-byte aggregate has no scalar width; its spill slot is padded to a word, so the
        // whole word is the lossless move. The scalar buckets below would keep one byte of it.
        enc.MovRaxLoad(d);
        return;
    }
    if (runtimeSz == 16) {
        enc.MovRaxLoad(d);
        enc.MovR10Load(d + 8);
        enc.Byte(0x4C);
        enc.Byte(0x89);
        enc.Byte(0xD2); // mov rdx, r10
    }
    else if (IsFloat(t)) {
        if (t.kind == TypeRef::Kind::Float32) {
            enc.MovssXmm0Load(d);
        }
        else {
            enc.MovsdXmm0Load(d);
        }
    }
    else if (sz == 8 || sz == 0) {
        enc.MovRaxLoad(d);
    }
    else if (t.IsSigned()) {
        if (sz == 4) {
            enc.MovsxdRaxDword(d);
        }
        else if (sz == 2) {
            enc.MovsxRaxWord(d);
        }
        else {
            enc.MovsxRaxByte(d);
        }
    }
    else {
        if (sz == 4) {
            enc.MovEaxLoad(d);
        }
        else if (sz == 2) {
            enc.MovzxRaxWord(d);
        }
        else {
            enc.MovzxRaxByte(d);
        }
    }
}

void X86_64ModuleEmitter::LoadB(LirReg reg, const TypeRef &t) const {
    const auto &physicalRegisters = FramePlan().PhysicalRegisters();
    auto it = physicalRegisters.find(reg);
    if (it != physicalRegisters.end()) {
        enc.MovR10PhysReg(it->second);
        int sz = SizeOfRuntime(t);
        if (sz > 0 && sz < 8 && !IsAggregate(t)) {
            if (t.IsSigned()) {
                if (sz == 4)
                    enc.MovsxdR10r10d();
                else if (sz == 2)
                    enc.MovsxR10r10w();
                else
                    enc.MovsxR10r10b();
            }
            else {
                if (sz == 4)
                    enc.MovR10dR10d();
                else if (sz == 2)
                    enc.MovzxR10r10w();
                else
                    enc.MovzxR10r10b();
            }
        }
        return;
    }
    int sz = SizeOfRuntime(t);
    int32_t d = Disp(reg);
    if (IsAggregate(t) && sz > 0 && sz < 8 && sz != 1 && sz != 2 && sz != 4) {
        // The odd-width aggregate: the slot is padded to a word, so the word is the lossless move.
        enc.MovR10Load(d);
        return;
    }
    if (IsFloat(t)) {
        if (t.kind == TypeRef::Kind::Float32) {
            enc.MovssXmm1Load(d);
        }
        else {
            enc.MovsdXmm1Load(d);
        }
    }
    else if (sz == 8 || sz == 0) {
        enc.MovR10Load(d);
    }
    else if (t.IsSigned()) {
        if (sz == 4) {
            enc.MovsxdR10Dword(d);
        }
        else if (sz == 2) {
            enc.MovsxR10Word(d);
        }
        else {
            enc.MovsxR10Byte(d);
        }
    }
    else {
        if (sz == 4) {
            enc.MovR10dLoad(d);
        }
        else if (sz == 2) {
            enc.MovzxR10Word(d);
        }
        else {
            enc.MovzxR10Byte(d);
        }
    }
}

void X86_64ModuleEmitter::StoreStack(LirReg dst, const TypeRef &t) const {
    int sz = SizeOfRuntime(t);
    int runtimeSz = SizeOfRuntime(t);
    int32_t d = Disp(dst);
    if (runtimeSz == 16) {
        enc.MovRaxStore(d);
        enc.Byte(0x48);
        enc.Byte(0x89);
        enc.Byte(0x95);
        enc.Dword(static_cast<uint32_t>(d + 8)); // mov [rbp+disp+8], rdx
    }
    else if (IsFloat(t)) {
        if (t.kind == TypeRef::Kind::Float32) {
            enc.MovssXmm0Store(d);
        }
        else {
            enc.MovsdXmm0Store(d);
        }
    }
    else {
        int ss = (sz > 0) ? sz : 8;
        if (IsAggregate(t) && ss < 8 && ss != 1 && ss != 2 && ss != 4) {
            // The odd-width aggregate again: the slot is padded to a word, so store the word.
            ss = 8;
        }
        if (ss == 8) {
            enc.MovRaxStore(d);
        }
        else if (ss == 4) {
            enc.MovEaxStore(d);
        }
        else if (ss == 2) {
            enc.MovAxStore(d);
        }
        else {
            enc.MovAlStore(d);
        }
    }
}

void X86_64ModuleEmitter::LoadChunkFromR10(const int32_t offset, const int size) const {
    if (size == 8) {
        enc.Byte(0x49);
        enc.Byte(0x8B);
        enc.Byte(0x82); // mov rax, [r10 + disp32]
    }
    else if (size == 4) {
        enc.Byte(0x41);
        enc.Byte(0x8B);
        enc.Byte(0x82); // mov eax, [r10 + disp32]
    }
    else if (size == 2) {
        enc.Byte(0x41);
        enc.Byte(0x0F);
        enc.Byte(0xB7);
        enc.Byte(0x82); // movzx eax, word [r10 + disp32]
    }
    else {
        enc.Byte(0x41);
        enc.Byte(0x0F);
        enc.Byte(0xB6);
        enc.Byte(0x82); // movzx eax, byte [r10 + disp32]
    }
    enc.Dword(static_cast<uint32_t>(offset));
}

void X86_64ModuleEmitter::StoreChunkToR11(const int32_t offset, const int size) const {
    if (size == 8) {
        enc.Byte(0x49);
        enc.Byte(0x89);
        enc.Byte(0x83); // mov [r11 + disp32], rax
    }
    else if (size == 4) {
        enc.Byte(0x41);
        enc.Byte(0x89);
        enc.Byte(0x83); // mov [r11 + disp32], eax
    }
    else if (size == 2) {
        enc.Byte(0x66);
        enc.Byte(0x41);
        enc.Byte(0x89);
        enc.Byte(0x83); // mov [r11 + disp32], ax
    }
    else {
        enc.Byte(0x41);
        enc.Byte(0x88);
        enc.Byte(0x83); // mov [r11 + disp32], al
    }
    enc.Dword(static_cast<uint32_t>(offset));
}

void X86_64ModuleEmitter::CopyAggregateFromR10ToStack(const int32_t dstDisp, const int size) const {
    CopyAggregateFromR10(size, [&](const int32_t offset, const int chunkSize) {
        if (chunkSize == 8) {
            enc.MovRaxStore(dstDisp + offset);
        }
        else if (chunkSize == 4) {
            enc.MovEaxStore(dstDisp + offset);
        }
        else if (chunkSize == 2) {
            enc.MovAxStore(dstDisp + offset);
        }
        else {
            enc.MovAlStore(dstDisp + offset);
        }
    });
}

void X86_64ModuleEmitter::StoreHiddenReturnValue(const LirReg src, const TypeRef &t) const {
    enc.MovR11Load(-FramePlan().HiddenReturnOffset());
    if (IsRegPointerTo(src, t)) {
        const auto &physicalRegisters = FramePlan().PhysicalRegisters();
        auto it = physicalRegisters.find(src);
        if (it != physicalRegisters.end()) {
            enc.MovR10PhysReg(it->second);
        }
        else {
            enc.MovR10Load(Disp(src));
        }
    }
    else {
        enc.Byte(0x4C);
        enc.Byte(0x8D);
        enc.Byte(0x95);
        enc.Dword(static_cast<uint32_t>(Disp(src))); // lea r10, [rbp + disp32]
    }
    CopyAggregateFromR10(SizeOfRuntime(t),
                         [&](const int32_t offset, const int size) { StoreChunkToR11(offset, size); });
    enc.Byte(0x4C);
    enc.Byte(0x89);
    enc.Byte(0xD8); // mov rax, r11
}
} // namespace Rux::X86_64Detail
