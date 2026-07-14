// Static Mach-O object writer for macOS (x86-64, ad-hoc code-signed).

#include "Linker/Linker.h"
#include "Linker/LinkerInternal.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Rux {
static std::optional<Buf> MacCompatThunk(const std::string &name) {
    // Helper: builds a syscall thunk from the register-shuffle body and
    // appends the common carry-flag error handling (jnc +3; neg rax; ret).
    static const auto MacSyscallThunk = [](std::initializer_list<uint8_t> body) -> Buf {
        Buf r(body);
        static constexpr uint8_t kErrTail[] = {0x73, 0x03, 0x48, 0xF7, 0xD8, 0xC3};
        r.insert(r.end(), kErrTail, kErrTail + sizeof(kErrTail));
        return r;
    };
    static const std::unordered_map<std::string, Buf> thunks = {
        {"ExitProcess",
         {
             0x48, 0x89,
             0xCF, // mov rdi, rcx  (exit code)
             0xB8, 0x01, 0x00, 0x00,
             0x02, // mov eax, 0x2000001 (SYS_exit)
             0x0F,
             0x05 // syscall
         }},
        {"GetStdHandle",
         {
             0x81, 0xF9, 0xF6, 0xFF, 0xFF,
             0xFF,       // cmp ecx, -10 (STD_INPUT_HANDLE)
             0x74, 0x0E, // je +14 (return 0)
             0x81, 0xF9, 0xF5, 0xFF, 0xFF,
             0xFF,                         // cmp ecx, -11 (STD_OUTPUT_HANDLE)
             0x74, 0x09,                   // je +9 (return 1)
             0xB8, 0x02, 0x00, 0x00, 0x00, // mov eax, 2 (stderr)
             0xC3,                         // ret
             0x31, 0xC0,                   // xor eax, eax (stdin = fd 0)
             0xC3,                         // ret
             0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1 (stdout = fd 1)
             0xC3,                         // ret
         }},
        {"GetProcessHeap", {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}},
        {"HeapFree", {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}},
        // HeapAlloc(heap, flags, size) -> mmap(NULL, size, RW,
        // MAP_PRIVATE|MAP_ANON, -1, 0). BSD MAP_PRIVATE|MAP_ANON = 0x1002;
        // macOS SYS_mmap = 0x20000C5.
        {"HeapAlloc",
         {
             0x4C, 0x89,
             0xC6, // mov rsi, r8  (size)
             0x31,
             0xFF, // xor edi, edi (addr = NULL)
             0xBA, 0x03, 0x00, 0x00,
             0x00, // mov edx, 3
             // (PROT_READ|PROT_WRITE)
             0x41, 0xBA, 0x02, 0x10, 0x00,
             0x00, // mov r10d, 0x1002
             // (MAP_PRIVATE|MAP_ANON)
             0x49, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF,
             0xFF, // mov r8, -1 (fd)
             0x45, 0x31,
             0xC9, // xor r9d, r9d (offset 0)
             0xB8, 0xC5, 0x00, 0x00,
             0x02, // mov eax, 0x20000C5 (SYS_mmap)
             0x0F,
             0x05, // syscall
             0xC3  // ret
         }},
        // HeapReAlloc(heap, flags, ptr, newSize): crude — fresh mmap of
        // newSize, no copy.
        {"HeapReAlloc",
         {
             0x48, 0x8B, 0x74, 0x24,
             0x28, // mov rsi, [rsp+40] (newSize,
             // 4th Win64 stack arg)
             0x31,
             0xFF, // xor edi, edi
             0xBA, 0x03, 0x00, 0x00,
             0x00, // mov edx, 3
             0x41, 0xBA, 0x02, 0x10, 0x00,
             0x00, // mov r10d, 0x1002
             0x49, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF,
             0xFF, // mov r8, -1
             0x45, 0x31,
             0xC9, // xor r9d, r9d
             0xB8, 0xC5, 0x00, 0x00,
             0x02, // mov eax, 0x20000C5 (SYS_mmap)
             0x0F,
             0x05, // syscall
             0xC3  // ret
         }},
        // Munmap(addr, length) -> munmap(addr, length). macOS SYS_munmap =
        // 0x2000049.
        {"Munmap",
         {
             0x48, 0x89,
             0xCF, // mov rdi, rcx (addr)
             0x48, 0x89,
             0xD6, // mov rsi, rdx (length)
             0xB8, 0x49, 0x00, 0x00,
             0x02, // mov eax, 0x2000049 (SYS_munmap)
             0x0F,
             0x05, // syscall
             0xC3  // ret
         }},
        {"RtlCopyMemory", {0x4D, 0x85, 0xC0, 0x74, 0x0F, 0x8A, 0x02, 0x88, 0x01, 0x48, 0xFF,
                           0xC2, 0x48, 0xFF, 0xC1, 0x49, 0xFF, 0xC8, 0x75, 0xF1, 0xC3}},
        {"RtlFillMemory",
         {0x48, 0x85, 0xD2, 0x74, 0x0B, 0x44, 0x88, 0x01, 0x48, 0xFF, 0xC1, 0x48, 0xFF, 0xCA, 0x75, 0xF5, 0xC3}},
        {"RtlZeroMemory", {0x45, 0x31, 0xC0, 0x48, 0x85, 0xD2, 0x74, 0x0B, 0x44, 0x88,
                           0x01, 0x48, 0xFF, 0xC1, 0x48, 0xFF, 0xCA, 0x75, 0xF5, 0xC3}},
        {"MultiByteToWideChar", {0x4C, 0x89, 0xC8, 0x4C, 0x8B, 0x54, 0x24, 0x28, 0x4D, 0x85, 0xD2, 0x74, 0x19,
                                 0x4D, 0x85, 0xC9, 0x7E, 0x14, 0x45, 0x0F, 0xB6, 0x18, 0x66, 0x45, 0x89, 0x1A,
                                 0x49, 0xFF, 0xC0, 0x49, 0x83, 0xC2, 0x02, 0x49, 0xFF, 0xC9, 0x75, 0xEC, 0xC3}},
        // WriteConsoleW(handle, buf, count, ...) -> per WCHAR, write(1,
        // lowByte, 1).
        {"WriteConsoleW",
         {
             0x41, 0x54,                   // push r12
             0x41, 0x55,                   // push r13
             0x48, 0x83, 0xEC, 0x08,       // sub rsp, 8
             0x49, 0x89, 0xD4,             // mov r12, rdx (buffer)
             0x4D, 0x89, 0xC5,             // mov r13, r8  (char count)
             0x4D, 0x85, 0xED,             // test r13, r13
             0x74, 0x24,                   // jz +36 (epilogue)
             0x41, 0x8A, 0x04, 0x24,       // mov al, [r12]
             0x88, 0x04, 0x24,             // mov [rsp], al
             0xB8, 0x04, 0x00, 0x00, 0x02, // mov eax, 0x2000004 (SYS_write)
             0xBF, 0x01, 0x00, 0x00, 0x00, // mov edi, 1 (stdout)
             0x48, 0x89, 0xE6,             // mov rsi, rsp
             0xBA, 0x01, 0x00, 0x00, 0x00, // mov edx, 1
             0x0F, 0x05,                   // syscall
             0x49, 0x83, 0xC4, 0x02,       // add r12, 2 (next WCHAR)
             0x49, 0xFF, 0xCD,             // dec r13
             0xEB, 0xD7,                   // jmp loop
             0x48, 0x83, 0xC4, 0x08,       // add rsp, 8
             0x41, 0x5D,                   // pop r13
             0x41, 0x5C,                   // pop r12
             0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1 (TRUE)
             0xC3                          // ret
         }},
        // ReadFile(handle, buf, count, *bytesRead) -> read(fd, buf, count).
        // macOS sets the carry flag on syscall error (errno in rax), so
        // branch on carry.
        {"ReadFile",
         {
             0x89, 0xCF,                   // mov edi, ecx (fd)
             0x48, 0x89, 0xD6,             // mov rsi, rdx (buf)
             0x4C, 0x89, 0xC2,             // mov rdx, r8  (count)
             0xB8, 0x03, 0x00, 0x00, 0x02, // mov eax, 0x2000003 (SYS_read)
             0x0F, 0x05,                   // syscall
             0x72, 0x0E,                   // jc +14 (error)
             0x4D, 0x85, 0xC9,             // test r9, r9 (null check)
             0x74, 0x03,                   // jz +3 (skip if null)
             0x41, 0x89, 0x01,             // mov [r9], eax (*bytesRead = result)
             0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1 (TRUE)
             0xC3,                         // ret
             0x31, 0xC0,                   // xor eax, eax (FALSE)
             0xC3                          // ret
         }},
        // WriteFile(handle, buf, count, *bytesWritten, overlapped) ->
        // write(fd, buf, count). Same shape as ReadFile; SYS_write instead
        // of SYS_read.
        {"WriteFile",
         {
             0x89, 0xCF,                   // mov edi, ecx  (fd)
             0x48, 0x89, 0xD6,             // mov rsi, rdx  (buf)
             0x4C, 0x89, 0xC2,             // mov rdx, r8   (count)
             0xB8, 0x04, 0x00, 0x00, 0x02, // mov eax, 0x2000004 (SYS_write)
             0x0F, 0x05,                   // syscall
             0x72, 0x0E,                   // jc +14 (error)
             0x4D, 0x85, 0xC9,             // test r9, r9 (null check)
             0x74, 0x03,                   // jz +3 (skip if null)
             0x41, 0x89, 0x01,             // mov [r9], eax (*bytesWritten = result)
             0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1 (TRUE)
             0xC3,                         // ret
             0x31, 0xC0,                   // xor eax, eax (FALSE)
             0xC3                          // ret
         }},
        // Raw syscall thunks for macOS. Same register shuffle as linux but with
        // carry-flag handling: macOS sets carry on error (positive errno in rax),
        // so we negate the result to match the Linux convention (negative errno).
        // The common tail (jnc +3; neg rax; ret) is appended by MacSyscallThunk.
        {"__rux_macos_syscall0", MacSyscallThunk({
                                     0x48,
                                     0x89,
                                     0xC8, // mov rax, rcx
                                     0x0F,
                                     0x05, // syscall
                                 })},
        {"__rux_macos_syscall1", MacSyscallThunk({
                                     0x48,
                                     0x89,
                                     0xC8, // mov rax, rcx
                                     0x48,
                                     0x89,
                                     0xD7, // mov rdi, rdx
                                     0x0F,
                                     0x05, // syscall
                                 })},
        {"__rux_macos_syscall2", MacSyscallThunk({
                                     0x48,
                                     0x89,
                                     0xC8, // mov rax, rcx
                                     0x48,
                                     0x89,
                                     0xD7, // mov rdi, rdx
                                     0x4C,
                                     0x89,
                                     0xC6, // mov rsi, r8
                                     0x0F,
                                     0x05, // syscall
                                 })},
        {"__rux_macos_syscall3", MacSyscallThunk({
                                     0x48,
                                     0x89,
                                     0xC8, // mov rax, rcx
                                     0x48,
                                     0x89,
                                     0xD7, // mov rdi, rdx
                                     0x4C,
                                     0x89,
                                     0xC6, // mov rsi, r8
                                     0x4C,
                                     0x89,
                                     0xCA, // mov rdx, r9
                                     0x0F,
                                     0x05, // syscall
                                 })},
        {"__rux_macos_syscall4", MacSyscallThunk({
                                     0x48, 0x89, 0xC8,             // mov rax, rcx
                                     0x48, 0x89, 0xD7,             // mov rdi, rdx
                                     0x4C, 0x89, 0xC6,             // mov rsi, r8
                                     0x4C, 0x89, 0xCA,             // mov rdx, r9
                                     0x4C, 0x8B, 0x54, 0x24, 0x28, // mov r10, [rsp + 40]
                                     0x0F, 0x05,                   // syscall
                                 })},
        {"__rux_macos_syscall5", MacSyscallThunk({
                                     0x48, 0x89, 0xC8,             // mov rax, rcx
                                     0x48, 0x89, 0xD7,             // mov rdi, rdx
                                     0x4C, 0x89, 0xC6,             // mov rsi, r8
                                     0x4C, 0x89, 0xCA,             // mov rdx, r9
                                     0x4C, 0x8B, 0x54, 0x24, 0x28, // mov r10, [rsp + 40]
                                     0x4C, 0x8B, 0x44, 0x24, 0x30, // mov r8, [rsp + 48]
                                     0x0F, 0x05,                   // syscall
                                 })},
        {"__rux_macos_syscall6", MacSyscallThunk({
                                     0x48, 0x89, 0xC8,             // mov rax, rcx
                                     0x48, 0x89, 0xD7,             // mov rdi, rdx
                                     0x4C, 0x89, 0xC6,             // mov rsi, r8
                                     0x4C, 0x89, 0xCA,             // mov rdx, r9
                                     0x4C, 0x8B, 0x54, 0x24, 0x28, // mov r10, [rsp + 40]
                                     0x4C, 0x8B, 0x44, 0x24, 0x30, // mov r8, [rsp + 48]
                                     0x4C, 0x8B, 0x4C, 0x24, 0x38, // mov r9, [rsp + 56]
                                     0x0F, 0x05,                   // syscall
                                 })},
    };

    const auto it = thunks.find(name);
    if (it == thunks.end()) {
        return std::nullopt;
    }
    return it->second;
}

// Links RcuFile objects into a static x86-64 Mach-O executable. No dyld:
// the kernel jumps straight to our entry via LC_UNIXTHREAD, and all OS
// interaction goes through raw syscalls in the compat thunks (same model as
// LinkElf64). The result is ad-hoc code-signed because Apple Silicon
// refuses to run any unsigned binary (including x86-64 ones translated by
// Rosetta 2).
bool Linker::LinkMachO64(const std::filesystem::path &outputPath) {
    static constexpr uint64_t kBase = 0x1'0000'0000ULL; // __TEXT base (after 4 GiB __PAGEZERO)
    static constexpr uint64_t kPage = 0x1000;

    const auto alignUp64 = [](const uint64_t v, const uint64_t a) { return (v + a - 1) & ~(a - 1); };

    // 1. Resolve externs: each must be satisfiable by a compat thunk.
    std::unordered_set<std::string> definedSymbols;
    for (const auto &obj : objects) {
        for (const auto &sym : obj.symbols) {
            if (sym.kind != RcuSymKind::ExternFunc && sym.kind != RcuSymKind::ExternData && !sym.name.empty()) {
                definedSymbols.insert(sym.name);
            }
        }
    }

    std::unordered_set<std::string> macCompatExterns;
    for (const auto &obj : objects) {
        for (const auto &sec : obj.sections) {
            for (const auto &reloc : sec.relocs) {
                if (reloc.symbolIndex >= obj.symbols.size()) {
                    continue;
                }
                const auto &sym = obj.symbols[reloc.symbolIndex];
                if ((sym.kind == RcuSymKind::ExternFunc || sym.kind == RcuSymKind::ExternData) &&
                    !definedSymbols.contains(sym.name)) {
                    if (MacCompatThunk(sym.name)) {
                        macCompatExterns.insert(sym.name);
                    }
                    else {
                        Error("external symbol '" + sym.name +
                              "' is not supported by the macOS Mach-O "
                              "linker yet");
                    }
                }
            }
        }
    }
    if (!errors.empty()) {
        return false;
    }

    // 2. Entry preamble: call Main; exit(eax).
    Buf textPre;
    textPre.insert(textPre.end(), {0x48, 0x83, 0xE4, 0xF0}); // and rsp, -16 (align stack)
    textPre.insert(textPre.end(), {0x48, 0x83, 0xEC, 0x08}); // sub rsp, 8 (16-byte align after call)
    const size_t kCallMainDisp = textPre.size() + 1;
    textPre.insert(textPre.end(), {0xE8, 0x00, 0x00, 0x00, 0x00}); // call Main
    textPre.insert(textPre.end(), {0x48, 0x83, 0xC4, 0x08});       // add rsp, 8 (undo sub)
    textPre.insert(textPre.end(), {0x89, 0xC7});                   // mov edi, eax (exit code)
    textPre.insert(textPre.end(), {0xB8, 0x01, 0x00, 0x00, 0x02}); // mov eax, 0x2000001 (SYS_exit)
    textPre.insert(textPre.end(), {0x0F, 0x05});                   // syscall

    // 3. Append compat thunks after the preamble (sorted for determinism).
    std::unordered_map<std::string, uint32_t> macCompatThunkOff;
    std::vector<std::string> macCompatNames(macCompatExterns.begin(), macCompatExterns.end());
    std::ranges::sort(macCompatNames);
    for (const auto &name : macCompatNames) {
        auto thunk = MacCompatThunk(name);
        if (!thunk) {
            continue;
        }
        macCompatThunkOff[name] = static_cast<uint32_t>(textPre.size());
        textPre.insert(textPre.end(), thunk->begin(), thunk->end());
    }
    const auto preambleSize = static_cast<uint32_t>(textPre.size());

    // 4. Merge per-object sections.
    struct ObjLayout {
        uint32_t textOff, rodataOff, dataOff;
    };

    std::vector<ObjLayout> layouts(objects.size());
    Buf mergedText, mergedRodata, mergedData;
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];
        layouts[i] = {static_cast<uint32_t>(mergedText.size()), static_cast<uint32_t>(mergedRodata.size()),
                      static_cast<uint32_t>(mergedData.size())};
        for (const auto &sec : obj.sections) {
            if (sec.type == RcuSecType::Text) {
                mergedText.insert(mergedText.end(), sec.data.begin(), sec.data.end());
            }
            else if (sec.type == RcuSecType::RoData) {
                mergedRodata.insert(mergedRodata.end(), sec.data.begin(), sec.data.end());
            }
            else if (sec.type == RcuSecType::Data) {
                mergedData.insert(mergedData.end(), sec.data.begin(), sec.data.end());
            }
        }
    }

    Buf textBuf;
    textBuf.insert(textBuf.end(), textPre.begin(), textPre.end());
    textBuf.insert(textBuf.end(), mergedText.begin(), mergedText.end());

    // 5. Fixed load-command set keeps header size constant:
    //    __PAGEZERO, __TEXT(__text,__const), __DATA(__data), __LINKEDIT,
    //    LC_UNIXTHREAD.
    constexpr uint32_t kSegCmd = 72;     // segment_command_64 (no trailing sections)
    constexpr uint32_t kSect = 80;       // section_64
    constexpr uint32_t kThreadCmd = 184; // LC_UNIXTHREAD with x86_THREAD_STATE64 (count 42)
    constexpr uint32_t kNCmds = 5;
    constexpr uint32_t sizeOfCmds = kSegCmd +               // __PAGEZERO
                                    (kSegCmd + 2 * kSect) + // __TEXT
                                    (kSegCmd + 1 * kSect) + // __DATA
                                    kSegCmd +               // __LINKEDIT
                                    kThreadCmd;
    constexpr uint64_t headerSize = 32 + sizeOfCmds;

    // 6. File/VA layout. Invariant: every segment's VA == kBase + its file
    // offset,
    //    so the file is page-padded between segments to match vm sizes.
    //    Reserve slack after the load commands: `codesign` inserts a
    //    16-byte LC_CODE_SIGNATURE there, and without room it would
    //    overwrite __text.
    static constexpr uint64_t kCodeSigLcSlack = 32;
    constexpr uint64_t textOff = alignUp64(headerSize + kCodeSigLcSlack, 16);
    constexpr uint64_t textVA = kBase + textOff;
    const uint64_t rodataOff = alignUp64(textOff + textBuf.size(), 16);
    const uint64_t rodataVA = kBase + rodataOff;
    const uint64_t textSegFileEnd = rodataOff + mergedRodata.size();
    const uint64_t textSegVMSize = alignUp64(textSegFileEnd, kPage);

    const uint64_t dataOff = alignUp64(textSegFileEnd, kPage);
    const uint64_t dataVA = kBase + dataOff;
    const uint64_t dataVMSize = alignUp64(std::max<uint64_t>(mergedData.size(), 1), kPage);

    const uint64_t linkeditOff = dataOff + dataVMSize;
    const uint64_t linkeditVA = kBase + linkeditOff;

    // 7. Symbol VAs (thunks + defined symbols).
    std::unordered_map<std::string, uint64_t> symMap;
    for (const auto &[name, off] : macCompatThunkOff) {
        symMap[name] = textVA + off;
    }

    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];
        const auto &lay = layouts[i];
        for (const auto &sym : obj.symbols) {
            if (sym.name.empty()) {
                continue;
            }
            if (sym.kind == RcuSymKind::ExternFunc || sym.kind == RcuSymKind::ExternData) {
                continue;
            }
            if (sym.visibility == RcuSymVis::Local && sym.kind != RcuSymKind::Func && sym.name != "Main") {
                continue;
            }

            uint64_t va = 0;
            if (sym.sectionIdx == RCU_TEXT_IDX) {
                va = textVA + preambleSize + lay.textOff + sym.value;
            }
            else if (sym.sectionIdx == RCU_RODATA_IDX) {
                va = rodataVA + lay.rodataOff + sym.value;
            }
            else if (sym.sectionIdx == RCU_DATA_IDX) {
                va = dataVA + lay.dataOff + sym.value;
            }
            else {
                continue;
            }
            symMap.try_emplace(sym.name, va);
        }
    }

    {
        auto it = symMap.find("Main");
        if (it == symMap.end()) {
            Error("undefined symbol 'Main' — no entry point found");
            return false;
        }
        const uint64_t nextInst = textVA + kCallMainDisp + 4;
        Patch32(textBuf, kCallMainDisp, static_cast<uint32_t>(it->second - nextInst));
    }

    // 8. Apply relocations (identical logic to LinkElf64, Mach-O VAs).
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];
        const auto &lay = layouts[i];
        for (const auto &sec : obj.sections) {
            Buf *buf = nullptr;
            uint32_t baseInBuf = 0;
            uint64_t secBaseVA = 0;
            if (sec.type == RcuSecType::Text) {
                buf = &textBuf;
                baseInBuf = preambleSize + lay.textOff;
                secBaseVA = textVA + preambleSize + lay.textOff;
            }
            else if (sec.type == RcuSecType::RoData) {
                buf = &mergedRodata;
                baseInBuf = lay.rodataOff;
                secBaseVA = rodataVA + lay.rodataOff;
            }
            else if (sec.type == RcuSecType::Data) {
                buf = &mergedData;
                baseInBuf = lay.dataOff;
                secBaseVA = dataVA + lay.dataOff;
            }
            else {
                continue;
            }

            for (const auto &reloc : sec.relocs) {
                if (reloc.symbolIndex >= obj.symbols.size()) {
                    continue;
                }
                const auto &sym = obj.symbols[reloc.symbolIndex];
                uint64_t targetVA = 0;
                if (sym.kind == RcuSymKind::ExternFunc || sym.kind == RcuSymKind::ExternData) {
                    auto it = symMap.find(sym.name);
                    if (it == symMap.end()) {
                        Error("undefined external symbol '" + sym.name + "'");
                        continue;
                    }
                    targetVA = it->second;
                }
                else if (sym.visibility != RcuSymVis::Local && !sym.name.empty() && symMap.contains(sym.name)) {
                    targetVA = symMap[sym.name];
                }
                else if (sym.sectionIdx == RCU_TEXT_IDX) {
                    targetVA = textVA + preambleSize + lay.textOff + sym.value;
                }
                else if (sym.sectionIdx == RCU_RODATA_IDX) {
                    targetVA = rodataVA + lay.rodataOff + sym.value;
                }
                else if (sym.sectionIdx == RCU_DATA_IDX) {
                    targetVA = dataVA + lay.dataOff + sym.value;
                }
                else {
                    continue;
                }

                const size_t patchAt = baseInBuf + reloc.sectionOffset;
                const uint64_t siteVA = secBaseVA + reloc.sectionOffset;
                if (reloc.type == RcuRelType::Rel32) {
                    if (patchAt + 4 > buf->size()) {
                        continue;
                    }
                    const auto disp = static_cast<int32_t>(targetVA + reloc.addend - (siteVA + 4));
                    Patch32(*buf, patchAt, static_cast<uint32_t>(disp));
                }
                else if (reloc.type == RcuRelType::Abs64) {
                    if (patchAt + 8 > buf->size()) {
                        continue;
                    }
                    Patch64(*buf, patchAt, targetVA + static_cast<uint64_t>(reloc.addend));
                }
                else if (reloc.type == RcuRelType::Abs32) {
                    if (patchAt + 4 > buf->size()) {
                        continue;
                    }
                    Patch32(*buf, patchAt, static_cast<uint32_t>(targetVA + reloc.addend));
                }
            }
        }
    }
    if (!errors.empty()) {
        return false;
    }

    // 9. Build the load commands.
    const auto wSegName = [](Buf &b, const char *s) {
        // Mach-O segment/section names are a fixed 16-byte, zero-padded field
        // (not necessarily NUL-terminated). Copy up to 16 bytes; the rest stay
        // zero.
        char n[16] = {};
        for (size_t i = 0; i < sizeof(n) && s[i] != '\0'; ++i) {
            n[i] = s[i];
        }
        for (const char c : n) {
            b.push_back(static_cast<uint8_t>(c));
        }
    };

    Buf lc;
    // __PAGEZERO
    WriteU32(lc, 0x19); // LC_SEGMENT_64
    WriteU32(lc, kSegCmd);
    wSegName(lc, "__PAGEZERO");
    WriteU64(lc, 0);     // vmaddr
    WriteU64(lc, kBase); // vmsize (4 GiB)
    WriteU64(lc, 0);     // fileoff
    WriteU64(lc, 0);     // filesize
    WriteU32(lc, 0);     // maxprot
    WriteU32(lc, 0);     // initprot
    WriteU32(lc, 0);     // nsects
    WriteU32(lc, 0);     // flags

    // __TEXT (R|X): header + __text + __const
    WriteU32(lc, 0x19);
    WriteU32(lc, kSegCmd + 2 * kSect);
    wSegName(lc, "__TEXT");
    WriteU64(lc, kBase); // vmaddr (segment maps from fileoff 0)
    WriteU64(lc, textSegVMSize);
    WriteU64(lc, 0);              // fileoff
    WriteU64(lc, textSegFileEnd); // filesize
    WriteU32(lc, 0x05);           // maxprot R|X
    WriteU32(lc, 0x05);           // initprot R|X
    WriteU32(lc, 2);              // nsects
    WriteU32(lc, 0);              // flags
    // section __text
    wSegName(lc, "__text");
    wSegName(lc, "__TEXT");
    WriteU64(lc, textVA);
    WriteU64(lc, textBuf.size());
    WriteU32(lc, static_cast<uint32_t>(textOff));
    WriteU32(lc, 4); // align 2^4
    WriteU32(lc, 0); // reloff
    WriteU32(lc, 0); // nreloc
    WriteU32(lc,
             0x8000'0400); // S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS
    WriteU32(lc, 0);
    WriteU32(lc, 0);
    WriteU32(lc, 0);
    // section __const (rodata)
    wSegName(lc, "__const");
    wSegName(lc, "__TEXT");
    WriteU64(lc, rodataVA);
    WriteU64(lc, mergedRodata.size());
    WriteU32(lc, static_cast<uint32_t>(rodataOff));
    WriteU32(lc, 4);
    WriteU32(lc, 0);
    WriteU32(lc, 0);
    WriteU32(lc, 0); // S_REGULAR
    WriteU32(lc, 0);
    WriteU32(lc, 0);
    WriteU32(lc, 0);

    // __DATA (R|W): __data
    WriteU32(lc, 0x19);
    WriteU32(lc, kSegCmd + 1 * kSect);
    wSegName(lc, "__DATA");
    WriteU64(lc, dataVA);
    WriteU64(lc, dataVMSize);
    WriteU64(lc, dataOff);
    WriteU64(lc, mergedData.size());
    WriteU32(lc, 0x03); // maxprot R|W
    WriteU32(lc, 0x03); // initprot R|W
    WriteU32(lc, 1);    // nsects
    WriteU32(lc, 0);
    // section __data
    wSegName(lc, "__data");
    wSegName(lc, "__DATA");
    WriteU64(lc, dataVA);
    WriteU64(lc, mergedData.size());
    WriteU32(lc, static_cast<uint32_t>(dataOff));
    WriteU32(lc, 0); // align 2^0
    WriteU32(lc, 0);
    WriteU32(lc, 0);
    WriteU32(lc, 0); // S_REGULAR
    WriteU32(lc, 0);
    WriteU32(lc, 0);
    WriteU32(lc, 0);

    // __LINKEDIT (empty placeholder; codesign grows it and adds
    // LC_CODE_SIGNATURE)
    WriteU32(lc, 0x19);
    WriteU32(lc, kSegCmd);
    wSegName(lc, "__LINKEDIT");
    WriteU64(lc, linkeditVA);
    WriteU64(lc, kPage);
    WriteU64(lc, linkeditOff);
    WriteU64(lc, 0);    // filesize
    WriteU32(lc, 0x01); // maxprot R
    WriteU32(lc, 0x01); // initprot R
    WriteU32(lc, 0);
    WriteU32(lc, 0);

    // LC_UNIXTHREAD (x86_THREAD_STATE64): set entry rip, leave rsp 0
    // (kernel default stack)
    WriteU32(lc, 0x05); // LC_UNIXTHREAD
    WriteU32(lc, kThreadCmd);
    WriteU32(lc, 4);  // flavor x86_THREAD_STATE64
    WriteU32(lc, 42); // count (21 uint64 registers = 42 uint32)
    for (int reg = 0; reg < 21; ++reg) {
        WriteU64(lc,
                 reg == 16 ? textVA : 0); // register 16 == rip == entry point
    }

    if (lc.size() != sizeOfCmds) {
        Error("internal: Mach-O load-command size mismatch");
        return false;
    }

    // 10. Emit the file.
    std::filesystem::create_directories(outputPath.parent_path());
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        Error("cannot open output file: " + outputPath.string());
        return false;
    }

    const auto writeRaw = [&](const void *d, size_t n) {
        out.write(static_cast<const char *>(d), static_cast<std::streamsize>(n));
    };
    const auto wBuf = [&](const Buf &b) {
        if (!b.empty()) {
            writeRaw(b.data(), b.size());
        }
    };
    const auto padToOffset = [&](uint64_t offset) {
        static constexpr uint8_t zeros[4096] = {};
        while (static_cast<uint64_t>(out.tellp()) < offset) {
            const uint64_t remaining = offset - static_cast<uint64_t>(out.tellp());
            writeRaw(zeros, static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(zeros))));
        }
    };

    Buf hdr;
    WriteU32(hdr, 0xFEED'FACF); // MH_MAGIC_64
    WriteU32(hdr, 0x0100'0007); // CPU_TYPE_X86_64
    WriteU32(hdr, 0x0000'0003); // CPU_SUBTYPE_X86_64_ALL
    WriteU32(hdr, 2);           // MH_EXECUTE
    WriteU32(hdr, kNCmds);
    WriteU32(hdr, sizeOfCmds);
    WriteU32(hdr, 0x0000'0001); // MH_NOUNDEFS
    WriteU32(hdr, 0);           // reserved
    wBuf(hdr);
    wBuf(lc);
    padToOffset(textOff);
    wBuf(textBuf);
    padToOffset(rodataOff);
    wBuf(mergedRodata);
    padToOffset(dataOff);
    wBuf(mergedData);
    padToOffset(linkeditOff); // keep __LINKEDIT fileoff within the file

    out.close();
    if (!out) {
        Error("cannot write output file: " + outputPath.string());
        return false;
    }

    std::error_code ec;
    std::filesystem::permissions(outputPath,
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, ec);
    if (ec) {
        Error("cannot mark output executable: " + ec.message());
        return false;
    }

    // Ad-hoc sign in place. Apple Silicon SIGKILLs unsigned binaries,
    // including x86-64 ones run under Rosetta 2; on Intel this is harmless
    // but still valid.
    const std::string signCmd = "codesign --force --sign - \"" + outputPath.string() + "\" 2>/dev/null";
    if (std::system(signCmd.c_str()) != 0) {
        Error("ad-hoc codesign failed (need Xcode command line tools); "
              "binary will not run on "
              "Apple Silicon");
        return false;
    }

    return true;
}
} // namespace Rux
