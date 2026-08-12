// AArch64 encoder vector table: one line per (form, operands, expected word).
//
// Included by AArch64EncoderTests.cpp, which defines the fixture and the five
// macros used here — A64_VECTORS opens a test case, A64_GROUP a subcase,
// A64_ENCODES states the word one encoder call must produce, A64_EMITS the
// words a composite sequence must produce, and A64_REFUSES the status an
// operand combination must be refused with. A refusal is also asserted to emit
// nothing at all, so an unencodable operand can never be a truncated
// instruction.
//
// The table is a file of its own because it is data: it is read down the left
// column, and it is the one part of these tests that grows with every
// instruction the back end learns to emit.
//
// Where the words come from. Each test case names the encoding diagram in the
// ARM Architecture Reference Manual for A-profile that its words were read off
// and reproduces that diagram's fields below it, most significant first, so an
// expected word can be checked against the table with no manual in hand. A
// section is cited by title, and by number only where the number was recorded
// with the vectors, since the titles are stable across issues of the manual
// where the numbering is not; the composite sequences at the end have no
// diagram of their own and cite the instructions they are built from instead.
// Every word was also cross-checked against
// `llvm-mc -triple=aarch64 -show-encoding` on the same mnemonic, except the
// LDR (literal) forms, whose assembly needs a symbol and which went through
// `llvm-mc -filetype=obj` and `llvm-objdump` instead.
//
// Nothing here runs AArch64 code or shells out to anything: a vector is a call
// and a constant, so the table checks the same on every host.

// Data Processing -- Immediate: Add/subtract (immediate), C4.1.92.
// sf | op | S | 100010 | sh | imm12 | Rn | Rd
A64_VECTORS("AArch64 encodes add and subtract immediates") {
    A64_GROUP("64-bit") {
        A64_ENCODES(0x910000A3, AddImm(Xn(3), Xn(5), 0));
        A64_ENCODES(0x910004A3, AddImm(Xn(3), Xn(5), 1));
        A64_ENCODES(0xB10004A3, AddsImm(Xn(3), Xn(5), 1));
        A64_ENCODES(0xD10004A3, SubImm(Xn(3), Xn(5), 1));
        A64_ENCODES(0xF10004A3, SubsImm(Xn(3), Xn(5), 1));

        // The boundary of the unshifted field, and the first value above it
        // that the LSL #12 form can still reach.
        A64_ENCODES(0x913FFCA3, AddImm(Xn(3), Xn(5), 0xFFF));
        A64_ENCODES(0x914004A3, AddImm(Xn(3), Xn(5), 0x1000));
        A64_ENCODES(0xD17FFCA3, SubImm(Xn(3), Xn(5), 0xFFF000));
    }

    A64_GROUP("32-bit") {
        A64_ENCODES(0x110004A3, AddImm(Wn(3), Wn(5), 1));
        A64_ENCODES(0x313FFCA3, AddsImm(Wn(3), Wn(5), 4095));
        A64_ENCODES(0x514004A3, SubImm(Wn(3), Wn(5), 4096));
        A64_ENCODES(0x717FFCA3, SubsImm(Wn(3), Wn(5), 0xFFF000));
    }

    A64_GROUP("stack pointer and zero register") {
        A64_ENCODES(0xD10043FF, SubImm(Sp, Sp, 16));
        A64_ENCODES(0x910043FF, AddImm(Sp, Sp, 16));
        A64_ENCODES(0x914007FF, AddImm(Sp, Sp, 0x1000));
        A64_ENCODES(0x110007FF, AddImm(Wsp, Wsp, 1));
        // CMP Xn, #imm and CMN Xn, #imm: the flag-setting forms writing XZR.
        A64_ENCODES(0xF10007FF, SubsImm(Xzr, Sp, 1));
        A64_ENCODES(0xB10004BF, AddsImm(Xzr, Xn(5), 1));
        A64_ENCODES(0x313FFCBF, AddsImm(Wzr, Wn(5), 4095));
    }

    A64_GROUP("refusals") {
        // Between the two shift positions there is nothing.
        A64_REFUSES(InvalidImmediate, AddImm(Xn(3), Xn(5), 0x1001));
        A64_REFUSES(InvalidImmediate, AddImm(Xn(3), Xn(5), 0x1000000));
        // ADDS and SUBS read the stack pointer but cannot write it, while ADD
        // and SUB cannot write the zero register.
        A64_REFUSES(InvalidRegister, AddsImm(Sp, Xn(5), 0));
        A64_REFUSES(InvalidRegister, AddImm(Xzr, Xn(5), 0));
        A64_REFUSES(InvalidRegister, AddImm(Xn(3), Xzr, 0));
        // Mixed widths and the wrong register file.
        A64_REFUSES(InvalidRegister, AddImm(Xn(3), Wn(5), 0));
        A64_REFUSES(InvalidRegister, AddImm(Dn(3), Xn(5), 0));
    }
}

// Data Processing -- Immediate: Logical (immediate), C4.1.93.
// sf | opc | 100100 | N | immr | imms | Rn | Rd
A64_VECTORS("AArch64 encodes logical immediates") {
    A64_GROUP("64-bit") {
        A64_ENCODES(0x92401CA3, AndImm(Xn(3), Xn(5), 0xFF));
        A64_ENCODES(0xB2401CA3, OrrImm(Xn(3), Xn(5), 0xFF));
        A64_ENCODES(0xD2401CA3, EorImm(Xn(3), Xn(5), 0xFF));
        A64_ENCODES(0xF2401CA3, AndsImm(Xn(3), Xn(5), 0xFF));
        A64_ENCODES(0x924000A3, AndImm(Xn(3), Xn(5), 1));
        A64_ENCODES(0x92407CA3, AndImm(Xn(3), Xn(5), 0xFFFFFFFF));
        // The longest run the encoding can name: every bit of the register but
        // one, which is immr at its maximum and imms one below it.
        A64_ENCODES(0x927FF8A3, AndImm(Xn(3), Xn(5), 0xFFFFFFFFFFFFFFFEULL));
        // A pattern that only encodes because it replicates in 2-bit elements.
        A64_ENCODES(0xB200F0A3, OrrImm(Xn(3), Xn(5), 0x5555555555555555ULL));
    }

    A64_GROUP("32-bit") {
        A64_ENCODES(0x12001CA3, AndImm(Wn(3), Wn(5), 0xFF));
        A64_ENCODES(0x320000A3, OrrImm(Wn(3), Wn(5), 1));
        A64_ENCODES(0x52185CA3, EorImm(Wn(3), Wn(5), 0xFFFFFF00U));
        A64_ENCODES(0x721E2CA3, AndsImm(Wn(3), Wn(5), 0x3FFC));
        A64_ENCODES(0x120104A3, AndImm(Wn(3), Wn(5), 0x80000001U));
        A64_ENCODES(0x321F78A3, OrrImm(Wn(3), Wn(5), 0xFFFFFFFEU));
    }

    A64_GROUP("stack pointer and zero register") {
        // AND, ORR and EOR write SP; ANDS writes XZR for its TST alias.
        A64_ENCODES(0x92401CBF, AndImm(Sp, Xn(5), 0xFF));
        A64_ENCODES(0x12001CBF, AndImm(Wsp, Wn(5), 0xFF));
        A64_ENCODES(0x721E2CBF, AndsImm(Wzr, Wn(5), 0x3FFC));
    }

    A64_GROUP("refusals") {
        A64_REFUSES(InvalidImmediate, AndImm(Xn(3), Xn(5), 0));
        A64_REFUSES(InvalidImmediate, AndImm(Xn(3), Xn(5), 0xFFFFFFFFFFFFFFFFULL));
        A64_REFUSES(InvalidImmediate, OrrImm(Xn(3), Xn(5), 0x1234));
        // Wider than the 32-bit instruction can reach.
        A64_REFUSES(InvalidImmediate, AndImm(Wn(3), Wn(5), 0x1FFFFFFFFULL));
        A64_REFUSES(InvalidRegister, AndsImm(Sp, Xn(5), 0xFF));
        A64_REFUSES(InvalidRegister, AndImm(Xn(3), Sp, 0xFF));
        A64_REFUSES(InvalidRegister, AndImm(Xn(3), Wn(5), 0xFF));
    }
}

// Data Processing -- Immediate: Move wide (immediate), C4.1.94.
// sf | opc | 100101 | hw | imm16 | Rd
A64_VECTORS("AArch64 encodes the move-wide group at every halfword") {
    A64_GROUP("all four halfwords") {
        A64_ENCODES(0xD2800023, Movz(Xn(3), 1));
        A64_ENCODES(0xD2A24683, Movz(Xn(3), 0x1234, 16));
        A64_ENCODES(0xD2C24683, Movz(Xn(3), 0x1234, 32));
        A64_ENCODES(0xD2E24683, Movz(Xn(3), 0x1234, 48));
        A64_ENCODES(0x92800023, Movn(Xn(3), 1));
        A64_ENCODES(0x92E24683, Movn(Xn(3), 0x1234, 48));
        A64_ENCODES(0xF2A00023, Movk(Xn(3), 1, 16));
        A64_ENCODES(0xF2C24683, Movk(Xn(3), 0x1234, 32));
        // The immediate field full, in the halfword at the top of the register.
        A64_ENCODES(0xD2FFFFE3, Movz(Xn(3), 0xFFFF, 48));
        A64_ENCODES(0xF2FFFFE3, Movk(Xn(3), 0xFFFF, 48));
        A64_ENCODES(0x929FFFE3, Movn(Xn(3), 0xFFFF));
    }

    A64_GROUP("32-bit") {
        A64_ENCODES(0x52800003, Movz(Wn(3), 0));
        A64_ENCODES(0x529FFFE3, Movz(Wn(3), 0xFFFF));
        A64_ENCODES(0x12824683, Movn(Wn(3), 0x1234));
        A64_ENCODES(0x72A24683, Movk(Wn(3), 0x1234, 16));
    }

    A64_GROUP("refusals") {
        // A W register has no third or fourth halfword.
        A64_REFUSES(InvalidImmediate, Movz(Wn(3), 1, 32));
        A64_REFUSES(InvalidImmediate, Movz(Xn(3), 1, 64));
        // The shift names a halfword, not an arbitrary bit position.
        A64_REFUSES(InvalidImmediate, Movk(Xn(3), 1, 8));
        A64_REFUSES(InvalidRegister, Movz(Sp, 1));
    }
}

// Data Processing -- Immediate: PC-rel. addressing, C4.1.91.
// op | immlo | 10000 | immhi | Rd
A64_VECTORS("AArch64 encodes ADR and ADRP with a split immediate") {
    A64_GROUP("ADR reaches a megabyte either way") {
        A64_ENCODES(0x10000009, Adr(Xn(9), 0));
        A64_ENCODES(0x10000029, Adr(Xn(9), 4));
        A64_ENCODES(0x10FFFFE9, Adr(Xn(9), -4));
        A64_ENCODES(0x107FFFE9, Adr(Xn(9), 1048572));
        A64_ENCODES(0x10800009, Adr(Xn(9), -1048576));
    }

    A64_GROUP("ADRP counts pages") {
        A64_ENCODES(0x90000009, Adrp(Xn(9), 0));
        A64_ENCODES(0xB0000009, Adrp(Xn(9), 4096));
        A64_ENCODES(0xF0FFFFE9, Adrp(Xn(9), -4096));
        A64_ENCODES(0xF07FFFE9, Adrp(Xn(9), 4294963200));
        A64_ENCODES(0x90800009, Adrp(Xn(9), -4294967296));
    }

    A64_GROUP("refusals") {
        A64_REFUSES(OutOfRange, Adr(Xn(9), 1048576));
        A64_REFUSES(OutOfRange, Adr(Xn(9), -1048580));
        A64_REFUSES(Unaligned, Adrp(Xn(9), 2048));
        A64_REFUSES(OutOfRange, Adrp(Xn(9), 4294967296));
        A64_REFUSES(InvalidRegister, Adr(Wn(9), 0));
        A64_REFUSES(InvalidRegister, Adr(Sp, 0));
    }
}

// Data Processing -- Immediate: Bitfield, C4.1.95, and the aliases every
// one of the three instructions carries in C6.2.
// sf | opc | 100110 | N | immr | imms | Rn | Rd
A64_VECTORS("AArch64 encodes bitfield moves and their aliases") {
    A64_GROUP("the three instructions") {
        A64_ENCODES(0x93401CA3, Sbfm(Xn(3), Xn(5), 0, 7));
        A64_ENCODES(0xD3401CA3, Ubfm(Xn(3), Xn(5), 0, 7));
        A64_ENCODES(0xB3401CA3, Bfm(Xn(3), Xn(5), 0, 7));
        A64_ENCODES(0x9345FCA3, Sbfm(Xn(3), Xn(5), 5, 63));
        A64_ENCODES(0x13051CA3, Sbfm(Wn(3), Wn(5), 5, 7));
        A64_ENCODES(0x530000A3, Ubfm(Wn(3), Wn(5), 0, 0));
        // Both fields at the top of their range, at both widths.
        A64_ENCODES(0x937FFCA3, Sbfm(Xn(3), Xn(5), 63, 63));
        A64_ENCODES(0xD37F00A3, Ubfm(Xn(3), Xn(5), 63, 0));
        A64_ENCODES(0xB37FFCA3, Bfm(Xn(3), Xn(5), 63, 63));
        A64_ENCODES(0x131F7CA3, Sbfm(Wn(3), Wn(5), 31, 31));
    }

    A64_GROUP("shifts") {
        // LSL rotates by the complement of its shift, so #0 is a plain move
        // and #1 sits at the far end of the immr field.
        A64_ENCODES(0xD340FCA3, Lsl(Xn(3), Xn(5), 0));
        A64_ENCODES(0xD37FF8A3, Lsl(Xn(3), Xn(5), 1));
        A64_ENCODES(0xD37CECA3, Lsl(Xn(3), Xn(5), 4));
        A64_ENCODES(0xD34100A3, Lsl(Xn(3), Xn(5), 63));
        A64_ENCODES(0x530100A3, Lsl(Wn(3), Wn(5), 31));
        A64_ENCODES(0xD341FCA3, Lsr(Xn(3), Xn(5), 1));
        A64_ENCODES(0x53037CA3, Lsr(Wn(3), Wn(5), 3));
        A64_ENCODES(0x9341FCA3, Asr(Xn(3), Xn(5), 1));
        A64_ENCODES(0x131F7CA3, Asr(Wn(3), Wn(5), 31));
        // A shift of nothing at all, and one of all but one bit of the
        // register: the two ends of what the aliases fold into immr and imms.
        A64_ENCODES(0xD340FCA3, Lsr(Xn(3), Xn(5), 0));
        A64_ENCODES(0xD37FFCA3, Lsr(Xn(3), Xn(5), 63));
        A64_ENCODES(0x937FFCA3, Asr(Xn(3), Xn(5), 63));
    }

    A64_GROUP("field extraction and insertion") {
        A64_ENCODES(0xD34654A3, Ubfx(Xn(3), Xn(5), 6, 16));
        A64_ENCODES(0x934654A3, Sbfx(Xn(3), Xn(5), 6, 16));
        A64_ENCODES(0xB37A3CA3, Bfi(Xn(3), Xn(5), 6, 16));
        A64_ENCODES(0xB34654A3, Bfxil(Xn(3), Xn(5), 6, 16));
        A64_ENCODES(0x530320A3, Ubfx(Wn(3), Wn(5), 3, 6));
        A64_ENCODES(0x130334A3, Sbfx(Wn(3), Wn(5), 3, 11));
        A64_ENCODES(0x331D14A3, Bfi(Wn(3), Wn(5), 3, 6));
        A64_ENCODES(0x330320A3, Bfxil(Wn(3), Wn(5), 3, 6));
        // A field at bit 0 makes BFI and BFXIL the same instruction.
        A64_ENCODES(0xB34000A3, Bfi(Xn(3), Xn(5), 0, 1));
        A64_ENCODES(0xB34000A3, Bfxil(Xn(3), Xn(5), 0, 1));
        // A field as wide as the register, and one bit at the top of it.
        A64_ENCODES(0xD340FCA3, Ubfx(Xn(3), Xn(5), 0, 64));
        A64_ENCODES(0x53007CA3, Ubfx(Wn(3), Wn(5), 0, 32));
        A64_ENCODES(0xD37FFCA3, Ubfx(Xn(3), Xn(5), 63, 1));
        A64_ENCODES(0x131F7CA3, Sbfx(Wn(3), Wn(5), 31, 1));
        A64_ENCODES(0xB37FFCA3, Bfxil(Xn(3), Xn(5), 63, 1));
        A64_ENCODES(0x330100A3, Bfi(Wn(3), Wn(5), 31, 1));
    }

    A64_GROUP("extensions") {
        A64_ENCODES(0x93401CA3, Sxtb(Xn(3), Wn(5)));
        A64_ENCODES(0x93403CA3, Sxth(Xn(3), Wn(5)));
        A64_ENCODES(0x93407CA3, Sxtw(Xn(3), Wn(5)));
        A64_ENCODES(0x13001CA3, Sxtb(Wn(3), Wn(5)));
        A64_ENCODES(0x13003CA3, Sxth(Wn(3), Wn(5)));
        A64_ENCODES(0x53001CA3, Uxtb(Wn(3), Wn(5)));
        A64_ENCODES(0x53003CA3, Uxth(Wn(3), Wn(5)));
    }

    A64_GROUP("refusals") {
        A64_REFUSES(InvalidImmediate, Sbfm(Xn(3), Xn(5), 64, 0));
        A64_REFUSES(InvalidImmediate, Ubfm(Wn(3), Wn(5), 0, 32));
        A64_REFUSES(InvalidImmediate, Lsl(Xn(3), Xn(5), 64));
        A64_REFUSES(InvalidImmediate, Lsr(Wn(3), Wn(5), 32));
        // A field must have at least one bit and must fit the register.
        A64_REFUSES(InvalidImmediate, Ubfx(Xn(3), Xn(5), 0, 0));
        A64_REFUSES(InvalidImmediate, Ubfx(Xn(3), Xn(5), 60, 8));
        A64_REFUSES(InvalidImmediate, Bfi(Wn(3), Wn(5), 30, 4));
        // The source of an extension is always a W register, and UXTB and UXTH
        // have no 64-bit form.
        A64_REFUSES(InvalidRegister, Sxtb(Xn(3), Xn(5)));
        A64_REFUSES(InvalidRegister, Sxtw(Wn(3), Wn(5)));
        A64_REFUSES(InvalidRegister, Uxtb(Xn(3), Wn(5)));
        A64_REFUSES(InvalidRegister, Ubfm(Sp, Xn(5), 0, 7));
    }
}

// Data Processing -- Immediate: Extract, C4.1.96.
// sf | op21 | 100111 | N | o0 | Rm | imms | Rn | Rd
A64_VECTORS("AArch64 encodes EXTR and its ROR alias") {
    A64_ENCODES(0x93C704A3, Extr(Xn(3), Xn(5), Xn(7), 1));
    A64_ENCODES(0x93C7FCA3, Extr(Xn(3), Xn(5), Xn(7), 63));
    A64_ENCODES(0x13877CA3, Extr(Wn(3), Wn(5), Wn(7), 31));
    A64_ENCODES(0x93C700A3, Extr(Xn(3), Xn(5), Xn(7), 0));
    // ROR is the form that reads one register twice.
    A64_ENCODES(0x93C510A3, Ror(Xn(3), Xn(5), 4));
    A64_ENCODES(0x13850CA3, Ror(Wn(3), Wn(5), 3));
    A64_ENCODES(0x138500A3, Ror(Wn(3), Wn(5), 0));

    A64_REFUSES(InvalidImmediate, Extr(Xn(3), Xn(5), Xn(7), 64));
    A64_REFUSES(InvalidImmediate, Extr(Wn(3), Wn(5), Wn(7), 32));
    A64_REFUSES(InvalidRegister, Extr(Xn(3), Xn(5), Wn(7), 0));
    A64_REFUSES(InvalidRegister, Ror(Sp, Xn(5), 1));
}

// Data Processing -- Register: Add/subtract (shifted register).
// sf | op | S | 01011 | shift | 0 | Rm | imm6 | Rn | Rd
A64_VECTORS("AArch64 encodes shifted-register add and subtract") {
    A64_GROUP("64-bit") {
        A64_ENCODES(0x8B0700A3, Add(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0xAB0700A3, Adds(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0xCB0700A3, Sub(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0xEB0700A3, Subs(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0x8B0730A3, Add(Xn(3), Xn(5), Xn(7), A64ShiftKind::Lsl, 12));
        A64_ENCODES(0x8B47FCA3, Add(Xn(3), Xn(5), Xn(7), A64ShiftKind::Lsr, 63));
        A64_ENCODES(0x8B8704A3, Add(Xn(3), Xn(5), Xn(7), A64ShiftKind::Asr, 1));
        A64_ENCODES(0xCB87FCA3, Sub(Xn(3), Xn(5), Xn(7), A64ShiftKind::Asr, 63));
        A64_ENCODES(0xCB07FCA3, Sub(Xn(3), Xn(5), Xn(7), A64ShiftKind::Lsl, 63));
        // Code 31 is the zero register in every field of this form.
        A64_ENCODES(0x8B1F00A3, Add(Xn(3), Xn(5), Xzr));
        A64_ENCODES(0x8B0700BF, Add(Xzr, Xn(5), Xn(7)));
    }

    A64_GROUP("32-bit") {
        A64_ENCODES(0x0B0700A3, Add(Wn(3), Wn(5), Wn(7)));
        A64_ENCODES(0x2B077CA3, Adds(Wn(3), Wn(5), Wn(7), A64ShiftKind::Lsl, 31));
        A64_ENCODES(0x4B4704A3, Sub(Wn(3), Wn(5), Wn(7), A64ShiftKind::Lsr, 1));
        A64_ENCODES(0x6B877CA3, Subs(Wn(3), Wn(5), Wn(7), A64ShiftKind::Asr, 31));
    }

    A64_GROUP("aliases") {
        A64_ENCODES(0xEB0700BF, Cmp(Xn(5), Xn(7)));
        A64_ENCODES(0xEB070CBF, Cmp(Xn(5), Xn(7), A64ShiftKind::Lsl, 3));
        A64_ENCODES(0xAB0700BF, Cmn(Xn(5), Xn(7)));
        A64_ENCODES(0xCB0703E3, Neg(Xn(3), Xn(7)));
        A64_ENCODES(0xEB0703E3, Negs(Xn(3), Xn(7)));
        A64_ENCODES(0x4B070BE3, Neg(Wn(3), Wn(7), A64ShiftKind::Lsl, 2));
        A64_ENCODES(0x6B8710BF, Cmp(Wn(5), Wn(7), A64ShiftKind::Asr, 4));
    }

    A64_GROUP("refusals") {
        // The shift amount is a bit position within the register.
        A64_REFUSES(InvalidImmediate, Add(Xn(3), Xn(5), Xn(7), A64ShiftKind::Lsl, 64));
        A64_REFUSES(InvalidImmediate, Add(Wn(3), Wn(5), Wn(7), A64ShiftKind::Lsl, 32));
        // ROR has no encoding in the arithmetic forms.
        A64_REFUSES(InvalidImmediate, Add(Xn(3), Xn(5), Xn(7), A64ShiftKind::Ror, 1));
        A64_REFUSES(InvalidImmediate, Subs(Wn(3), Wn(5), Wn(7), A64ShiftKind::Ror, 0));
        // No field of this form reads code 31 as the stack pointer, so SP is
        // reachable only through the extended-register form.
        A64_REFUSES(InvalidRegister, Add(Sp, Xn(5), Xn(7)));
        A64_REFUSES(InvalidRegister, Add(Xn(3), Sp, Xn(7)));
        A64_REFUSES(InvalidRegister, Cmp(Sp, Xn(7)));
        A64_REFUSES(InvalidRegister, Add(Xn(3), Xn(5), Wn(7)));
        A64_REFUSES(InvalidRegister, Add(Xn(3), Xn(5), Dn(7)));
    }
}

// Data Processing -- Register: Add/subtract (extended register).
// sf | op | S | 01011 | 00 | 1 | Rm | option | imm3 | Rn | Rd
A64_VECTORS("AArch64 encodes extended-register add and subtract") {
    A64_GROUP("every option at every shift") {
        A64_ENCODES(0x8B2700A3, AddExt(Xn(3), Xn(5), Wn(7), A64ExtendKind::Uxtb));
        A64_ENCODES(0x8B2724A3, AddExt(Xn(3), Xn(5), Wn(7), A64ExtendKind::Uxth, 1));
        A64_ENCODES(0x8B2748A3, AddExt(Xn(3), Xn(5), Wn(7), A64ExtendKind::Uxtw, 2));
        A64_ENCODES(0x8B276CA3, AddExt(Xn(3), Xn(5), Xn(7), A64ExtendKind::Uxtx, 3));
        A64_ENCODES(0x8B2790A3, AddExt(Xn(3), Xn(5), Wn(7), A64ExtendKind::Sxtb, 4));
        A64_ENCODES(0xAB27A0A3, AddsExt(Xn(3), Xn(5), Wn(7), A64ExtendKind::Sxth));
        A64_ENCODES(0xCB27C0A3, SubExt(Xn(3), Xn(5), Wn(7), A64ExtendKind::Sxtw));
        A64_ENCODES(0xEB27E0A3, SubsExt(Xn(3), Xn(5), Xn(7), A64ExtendKind::Sxtx));
    }

    A64_GROUP("stack pointer operands") {
        A64_ENCODES(0x8B2763FF, AddExt(Sp, Sp, Xn(7), A64ExtendKind::Uxtx));
        A64_ENCODES(0x8B274BE3, AddExt(Xn(3), Sp, Wn(7), A64ExtendKind::Uxtw, 2));
        // CMP SP, Xm is the flag-setting form, which reads SP but writes XZR.
        A64_ENCODES(0xEB27E3FF, SubsExt(Xzr, Sp, Xn(7), A64ExtendKind::Sxtx));
        A64_ENCODES(0x0B2743FF, AddExt(Wsp, Wsp, Wn(7), A64ExtendKind::Uxtw));
    }

    A64_GROUP("32-bit") {
        A64_ENCODES(0x0B2700A3, AddExt(Wn(3), Wn(5), Wn(7), A64ExtendKind::Uxtb));
        // A 32-bit instruction has no doubleword source, so even UXTX reads a W
        // register.
        A64_ENCODES(0x2B2764A3, AddsExt(Wn(3), Wn(5), Wn(7), A64ExtendKind::Uxtx, 1));
    }

    A64_GROUP("refusals") {
        A64_REFUSES(InvalidImmediate, AddExt(Xn(3), Xn(5), Wn(7), A64ExtendKind::Uxtb, 5));
        // The extension names the width of the source.
        A64_REFUSES(InvalidRegister, AddExt(Xn(3), Xn(5), Xn(7), A64ExtendKind::Uxtb));
        A64_REFUSES(InvalidRegister, AddExt(Xn(3), Xn(5), Wn(7), A64ExtendKind::Uxtx));
        A64_REFUSES(InvalidRegister, AddExt(Wn(3), Wn(5), Xn(7), A64ExtendKind::Uxtx));
        // The flag-setting forms cannot write SP, and no form reads XZR as rn.
        A64_REFUSES(InvalidRegister, AddsExt(Sp, Sp, Xn(7), A64ExtendKind::Uxtx));
        A64_REFUSES(InvalidRegister, AddExt(Xn(3), Xzr, Wn(7), A64ExtendKind::Uxtb));
    }
}

// Data Processing -- Register: Logical (shifted register).
// sf | opc | 01010 | shift | N | Rm | imm6 | Rn | Rd
A64_VECTORS("AArch64 encodes the logical shifted-register group") {
    A64_GROUP("the eight operations") {
        A64_ENCODES(0x8A0700A3, And(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0x8A2700A3, Bic(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0xAA0700A3, Orr(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0xAA2700A3, Orn(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0xCA0700A3, Eor(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0xCA2700A3, Eon(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0xEA0700A3, Ands(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0xEA2700A3, Bics(Xn(3), Xn(5), Xn(7)));
    }

    A64_GROUP("shifts, including the ROR the arithmetic forms lack") {
        A64_ENCODES(0x8A07FCA3, And(Xn(3), Xn(5), Xn(7), A64ShiftKind::Lsl, 63));
        A64_ENCODES(0xAA4704A3, Orr(Xn(3), Xn(5), Xn(7), A64ShiftKind::Lsr, 1));
        A64_ENCODES(0xCA8780A3, Eor(Xn(3), Xn(5), Xn(7), A64ShiftKind::Asr, 32));
        A64_ENCODES(0x8AE740A3, Bic(Xn(3), Xn(5), Xn(7), A64ShiftKind::Ror, 16));
        A64_ENCODES(0x8AC7FCA3, And(Xn(3), Xn(5), Xn(7), A64ShiftKind::Ror, 63));
        A64_ENCODES(0x0A0700A3, And(Wn(3), Wn(5), Wn(7)));
        A64_ENCODES(0x6AC77CA3, Ands(Wn(3), Wn(5), Wn(7), A64ShiftKind::Ror, 31));
        A64_ENCODES(0x2A0704A3, Orr(Wn(3), Wn(5), Wn(7), A64ShiftKind::Lsl, 1));
    }

    A64_GROUP("aliases") {
        A64_ENCODES(0xEA0700BF, Tst(Xn(5), Xn(7)));
        A64_ENCODES(0x6A070CBF, Tst(Wn(5), Wn(7), A64ShiftKind::Lsl, 3));
        A64_ENCODES(0xAA2703E3, Mvn(Xn(3), Xn(7)));
        A64_ENCODES(0x2AA717E3, Mvn(Wn(3), Wn(7), A64ShiftKind::Asr, 5));
        A64_ENCODES(0xAA0703E3, Mov(Xn(3), Xn(7)));
        A64_ENCODES(0x2A0703E3, Mov(Wn(3), Wn(7)));
        A64_ENCODES(0xAA1F03E3, Mov(Xn(3), Xzr));
        // MOV naming SP is the arithmetic form, since ORR reads code 31 as XZR.
        A64_ENCODES(0x910000BF, Mov(Sp, Xn(5)));
        A64_ENCODES(0x910003E3, Mov(Xn(3), Sp));
        A64_ENCODES(0x110000BF, Mov(Wsp, Wn(5)));
    }

    A64_GROUP("refusals") {
        A64_REFUSES(InvalidImmediate, And(Xn(3), Xn(5), Xn(7), A64ShiftKind::Lsl, 64));
        A64_REFUSES(InvalidImmediate, Orr(Wn(3), Wn(5), Wn(7), A64ShiftKind::Ror, 32));
        A64_REFUSES(InvalidRegister, And(Sp, Xn(5), Xn(7)));
        A64_REFUSES(InvalidRegister, Tst(Sp, Xn(7)));
        A64_REFUSES(InvalidRegister, Orr(Xn(3), Xn(5), Wn(7)));
        // Neither reading of code 31 can be moved to the other.
        A64_REFUSES(InvalidRegister, Mov(Sp, Xzr));
        A64_REFUSES(InvalidRegister, Mov(Xzr, Sp));
        A64_REFUSES(InvalidRegister, Mov(Xn(3), Wn(7)));
    }
}

// Data Processing -- Register: Data-processing (2 source).
// sf | 0 | S | 11010110 | Rm | opcode | Rn | Rd
A64_VECTORS("AArch64 encodes variable shifts and division") {
    A64_ENCODES(0x9AC720A3, Lslv(Xn(3), Xn(5), Xn(7)));
    A64_ENCODES(0x9AC724A3, Lsrv(Xn(3), Xn(5), Xn(7)));
    A64_ENCODES(0x9AC728A3, Asrv(Xn(3), Xn(5), Xn(7)));
    A64_ENCODES(0x9AC72CA3, Rorv(Xn(3), Xn(5), Xn(7)));
    A64_ENCODES(0x1AC720A3, Lslv(Wn(3), Wn(5), Wn(7)));
    A64_ENCODES(0x1AC72CA3, Rorv(Wn(3), Wn(5), Wn(7)));

    A64_ENCODES(0x9AC70CA3, Sdiv(Xn(3), Xn(5), Xn(7)));
    A64_ENCODES(0x9AC708A3, Udiv(Xn(3), Xn(5), Xn(7)));
    A64_ENCODES(0x1AC70CA3, Sdiv(Wn(3), Wn(5), Wn(7)));
    A64_ENCODES(0x1AC708A3, Udiv(Wn(3), Wn(5), Wn(7)));

    A64_REFUSES(InvalidRegister, Lslv(Sp, Xn(5), Xn(7)));
    A64_REFUSES(InvalidRegister, Sdiv(Xn(3), Wn(5), Xn(7)));
    A64_REFUSES(InvalidRegister, Udiv(Xn(3), Xn(5), Sp));
}

// Data Processing -- Register: Data-processing (3 source).
// sf | op54 | 11011 | op31 | Rm | o0 | Ra | Rn | Rd
A64_VECTORS("AArch64 encodes the multiply-accumulate group") {
    A64_GROUP("same-width forms") {
        A64_ENCODES(0x9B072CA3, Madd(Xn(3), Xn(5), Xn(7), Xn(11)));
        A64_ENCODES(0x9B07ACA3, Msub(Xn(3), Xn(5), Xn(7), Xn(11)));
        A64_ENCODES(0x9B077CA3, Mul(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0x9B07FCA3, Mneg(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0x1B072CA3, Madd(Wn(3), Wn(5), Wn(7), Wn(11)));
        A64_ENCODES(0x1B077CA3, Mul(Wn(3), Wn(5), Wn(7)));
        A64_ENCODES(0x1B07FCA3, Mneg(Wn(3), Wn(5), Wn(7)));
    }

    A64_GROUP("widening and high-half forms") {
        A64_ENCODES(0x9B272CA3, Smaddl(Xn(3), Wn(5), Wn(7), Xn(11)));
        A64_ENCODES(0x9BA72CA3, Umaddl(Xn(3), Wn(5), Wn(7), Xn(11)));
        A64_ENCODES(0x9B277CA3, Smull(Xn(3), Wn(5), Wn(7)));
        A64_ENCODES(0x9BA77CA3, Umull(Xn(3), Wn(5), Wn(7)));
        A64_ENCODES(0x9B477CA3, Smulh(Xn(3), Xn(5), Xn(7)));
        A64_ENCODES(0x9BC77CA3, Umulh(Xn(3), Xn(5), Xn(7)));
    }

    A64_GROUP("refusals") {
        A64_REFUSES(InvalidRegister, Madd(Xn(3), Wn(5), Xn(7), Xn(11)));
        A64_REFUSES(InvalidRegister, Madd(Xn(3), Xn(5), Xn(7), Wn(11)));
        A64_REFUSES(InvalidRegister, Mul(Sp, Xn(5), Xn(7)));
        // The widening forms multiply W registers into an X register; nothing
        // else about their operand widths is negotiable.
        A64_REFUSES(InvalidRegister, Smull(Wn(3), Wn(5), Wn(7)));
        A64_REFUSES(InvalidRegister, Smull(Xn(3), Xn(5), Xn(7)));
        A64_REFUSES(InvalidRegister, Umaddl(Xn(3), Wn(5), Wn(7), Wn(11)));
        A64_REFUSES(InvalidRegister, Smulh(Xn(3), Wn(5), Wn(7)));
        A64_REFUSES(InvalidRegister, Umulh(Wn(3), Wn(5), Wn(7)));
    }
}

// Data Processing -- Register: Conditional select.
// sf | op | S | 11010100 | Rm | cond | op2 | Rn | Rd
A64_VECTORS("AArch64 encodes conditional selects and their aliases") {
    A64_GROUP("the four instructions") {
        A64_ENCODES(0x9A8700A3, Csel(Xn(3), Xn(5), Xn(7), A64Condition::Eq));
        A64_ENCODES(0x9A8714A3, Csinc(Xn(3), Xn(5), Xn(7), A64Condition::Ne));
        A64_ENCODES(0xDA87A0A3, Csinv(Xn(3), Xn(5), Xn(7), A64Condition::Ge));
        A64_ENCODES(0xDA87B4A3, Csneg(Xn(3), Xn(5), Xn(7), A64Condition::Lt));
        A64_ENCODES(0x1A8780A3, Csel(Wn(3), Wn(5), Wn(7), A64Condition::Hi));
        // The whole condition field, which only this group and the branches
        // below reach in full.
        A64_ENCODES(0x9A8720A3, Csel(Xn(3), Xn(5), Xn(7), A64Condition::Cs));
        A64_ENCODES(0x9A8730A3, Csel(Xn(3), Xn(5), Xn(7), A64Condition::Cc));
        A64_ENCODES(0x9A8740A3, Csel(Xn(3), Xn(5), Xn(7), A64Condition::Mi));
        A64_ENCODES(0x9A8750A3, Csel(Xn(3), Xn(5), Xn(7), A64Condition::Pl));
        A64_ENCODES(0x9A8760A3, Csel(Xn(3), Xn(5), Xn(7), A64Condition::Vs));
        A64_ENCODES(0x9A8770A3, Csel(Xn(3), Xn(5), Xn(7), A64Condition::Vc));
        A64_ENCODES(0x9A8790A3, Csel(Xn(3), Xn(5), Xn(7), A64Condition::Ls));
        A64_ENCODES(0x9A87C0A3, Csel(Xn(3), Xn(5), Xn(7), A64Condition::Gt));
        A64_ENCODES(0x9A87D0A3, Csel(Xn(3), Xn(5), Xn(7), A64Condition::Le));
        A64_ENCODES(0x9A87E4A3, Csinc(Xn(3), Xn(5), Xn(7), A64Condition::Al));
        A64_ENCODES(0x5A87F0A3, Csinv(Wn(3), Wn(5), Wn(7), A64Condition::Nv));
        A64_ENCODES(0x5A8704A3, Csneg(Wn(3), Wn(5), Wn(7), A64Condition::Eq));
        // The instructions themselves accept AL and NV; only the aliases cannot.
        A64_ENCODES(0x9A87E0A3, Csel(Xn(3), Xn(5), Xn(7), A64Condition::Al));
        A64_ENCODES(0x9A87F0A3, Csel(Xn(3), Xn(5), Xn(7), A64Condition::Nv));
    }

    A64_GROUP("aliases encode the inverse condition") {
        A64_ENCODES(0x9A9F17E3, Cset(Xn(3), A64Condition::Eq));
        A64_ENCODES(0x1A9F07E3, Cset(Wn(3), A64Condition::Ne));
        A64_ENCODES(0xDA9FA3E3, Csetm(Xn(3), A64Condition::Lt));
        A64_ENCODES(0x9A85D4A3, Cinc(Xn(3), Xn(5), A64Condition::Gt));
        A64_ENCODES(0xDA85C0A3, Cinv(Xn(3), Xn(5), A64Condition::Le));
        A64_ENCODES(0xDA8554A3, Cneg(Xn(3), Xn(5), A64Condition::Mi));
        A64_ENCODES(0x5A8544A3, Cneg(Wn(3), Wn(5), A64Condition::Pl));
    }

    A64_GROUP("refusals") {
        // Inverting AL gives NV, which also means always, so an alias written
        // with either would read as the opposite of itself.
        A64_REFUSES(InvalidImmediate, Cset(Xn(3), A64Condition::Al));
        A64_REFUSES(InvalidImmediate, Cset(Xn(3), A64Condition::Nv));
        A64_REFUSES(InvalidImmediate, Csetm(Wn(3), A64Condition::Al));
        A64_REFUSES(InvalidImmediate, Cinc(Xn(3), Xn(5), A64Condition::Nv));
        A64_REFUSES(InvalidImmediate, Cinv(Xn(3), Xn(5), A64Condition::Al));
        A64_REFUSES(InvalidImmediate, Cneg(Xn(3), Xn(5), A64Condition::Al));
        A64_REFUSES(InvalidRegister, Csel(Sp, Xn(5), Xn(7), A64Condition::Eq));
        A64_REFUSES(InvalidRegister, Csel(Xn(3), Wn(5), Xn(7), A64Condition::Eq));
        A64_REFUSES(InvalidRegister, Cset(Sp, A64Condition::Eq));
        A64_REFUSES(InvalidRegister, Csinc(Xn(3), Xn(5), Wn(7), A64Condition::Eq));
    }
}

// Data Processing -- Register: Data-processing (1 source).
// sf | 1 | S | 11010110 | opcode2 | opcode | Rn | Rd
A64_VECTORS("AArch64 encodes bit counting and byte reversal") {
    A64_ENCODES(0xDAC010A3, Clz(Xn(3), Xn(5)));
    A64_ENCODES(0xDAC014A3, Cls(Xn(3), Xn(5)));
    A64_ENCODES(0xDAC000A3, Rbit(Xn(3), Xn(5)));
    A64_ENCODES(0xDAC00CA3, Rev(Xn(3), Xn(5)));
    A64_ENCODES(0xDAC004A3, Rev16(Xn(3), Xn(5)));
    A64_ENCODES(0xDAC008A3, Rev32(Xn(3), Xn(5)));

    A64_ENCODES(0x5AC010A3, Clz(Wn(3), Wn(5)));
    A64_ENCODES(0x5AC014A3, Cls(Wn(3), Wn(5)));
    A64_ENCODES(0x5AC000A3, Rbit(Wn(3), Wn(5)));
    A64_ENCODES(0x5AC004A3, Rev16(Wn(3), Wn(5)));
    // At 32 bits REV occupies the opcode REV32 has at 64, since reversing the
    // bytes of the only word is reversing the whole register.
    A64_ENCODES(0x5AC008A3, Rev(Wn(3), Wn(5)));

    A64_REFUSES(InvalidRegister, Rev32(Wn(3), Wn(5)));
    A64_REFUSES(InvalidRegister, Clz(Xn(3), Wn(5)));
    A64_REFUSES(InvalidRegister, Rbit(Sp, Xn(5)));
    A64_REFUSES(InvalidRegister, Rev(Xn(3), Dn(5)));
}

// Loads and Stores: Load/store register (unsigned immediate).
// size | 111 | V | 01 | opc | imm12 | Rn | Rt
A64_VECTORS("AArch64 encodes unsigned-offset loads and stores") {
    A64_GROUP("general purpose, scaled by the access width") {
        A64_ENCODES(0xF9400020, Ldr(Xn(0), Xn(1)));
        A64_ENCODES(0xF9400420, Ldr(Xn(0), Xn(1), 8));
        A64_ENCODES(0xF97FFC20, Ldr(Xn(0), Xn(1), 32760));
        A64_ENCODES(0xF9000020, Str(Xn(0), Xn(1)));
        A64_ENCODES(0xF9000420, Str(Xn(0), Xn(1), 8));
        A64_ENCODES(0xF93FFC20, Str(Xn(0), Xn(1), 32760));
        A64_ENCODES(0xB9400020, Ldr(Wn(0), Xn(1)));
        A64_ENCODES(0xB9400420, Ldr(Wn(0), Xn(1), 4));
        A64_ENCODES(0xB97FFC20, Ldr(Wn(0), Xn(1), 16380));
        // The base is the one field that reads code 31 as the stack pointer.
        A64_ENCODES(0xF94007E0, Ldr(Xn(0), Sp, 8));
        A64_ENCODES(0xF940003F, Ldr(Xzr, Xn(1)));
    }

    A64_GROUP("all five SIMD widths") {
        A64_ENCODES(0x3D400C20, Ldr(Bn(0), Xn(1), 3));
        A64_ENCODES(0x3D7FFC20, Ldr(Bn(0), Xn(1), 4095));
        A64_ENCODES(0x7D400420, Ldr(Hn(0), Xn(1), 2));
        A64_ENCODES(0x7D7FFC20, Ldr(Hn(0), Xn(1), 8190));
        A64_ENCODES(0xBD400420, Ldr(Sn(0), Xn(1), 4));
        A64_ENCODES(0xFD400420, Ldr(Dn(0), Xn(1), 8));
        A64_ENCODES(0xBD7FFC20, Ldr(Sn(0), Xn(1), 16380));
        A64_ENCODES(0xFD7FFC20, Ldr(Dn(0), Xn(1), 32760));
        // A Q access has no `size` value of its own and borrows a bit of `opc`.
        A64_ENCODES(0x3DC00020, Ldr(Qn(0), Xn(1)));
        A64_ENCODES(0x3DFFFC20, Ldr(Qn(0), Xn(1), 65520));
        A64_ENCODES(0x3D800420, Str(Qn(0), Xn(1), 16));
    }

    A64_GROUP("narrowing and sign-extending") {
        A64_ENCODES(0x39400C20, Ldrb(Wn(0), Xn(1), 3));
        A64_ENCODES(0x397FFC20, Ldrb(Wn(0), Xn(1), 4095));
        A64_ENCODES(0x39000425, Strb(Wn(5), Xn(1), 1));
        A64_ENCODES(0x79400420, Ldrh(Wn(0), Xn(1), 2));
        A64_ENCODES(0x797FFC20, Ldrh(Wn(0), Xn(1), 8190));
        A64_ENCODES(0x790007E5, Strh(Wn(5), Sp, 2));
        // The sign-extending loads name the width they extend to, so the two
        // widths of LDRSB are two encodings.
        A64_ENCODES(0x39800420, Ldrsb(Xn(0), Xn(1), 1));
        A64_ENCODES(0x39C00427, Ldrsb(Wn(7), Xn(1), 1));
        A64_ENCODES(0x79800420, Ldrsh(Xn(0), Xn(1), 2));
        A64_ENCODES(0x79C00027, Ldrsh(Wn(7), Xn(1)));
        A64_ENCODES(0xB9800420, Ldrsw(Xn(0), Xn(1), 4));
        A64_ENCODES(0xB9BFFC20, Ldrsw(Xn(0), Xn(1), 16380));
        A64_ENCODES(0x39BFFC20, Ldrsb(Xn(0), Xn(1), 4095));
        A64_ENCODES(0x79BFFC20, Ldrsh(Xn(0), Xn(1), 8190));
    }

    A64_GROUP("refusals") {
        // An offset that does not divide by the access width has no encoding
        // here at all, and one past the field is out of range, not truncated.
        A64_REFUSES(Unaligned, Ldr(Xn(0), Xn(1), 4));
        A64_REFUSES(Unaligned, Ldr(Qn(0), Xn(1), 8));
        A64_REFUSES(OutOfRange, Ldr(Xn(0), Xn(1), 32768));
        A64_REFUSES(OutOfRange, Ldrb(Wn(0), Xn(1), 4096));
        // The base is always a 64-bit register, and never the zero register.
        A64_REFUSES(InvalidRegister, Ldr(Xn(0), Wn(1)));
        A64_REFUSES(InvalidRegister, Ldr(Xn(0), Xzr));
        // The transferred register never reads code 31 as the stack pointer.
        A64_REFUSES(InvalidRegister, Str(Sp, Xn(1)));
        // LDRB and LDRH name a W register; LDRSW only widens, so it names an X.
        A64_REFUSES(InvalidRegister, Ldrb(Xn(0), Xn(1)));
        A64_REFUSES(InvalidRegister, Strh(Xn(0), Xn(1)));
        A64_REFUSES(InvalidRegister, Ldrsw(Wn(0), Xn(1)));
        A64_REFUSES(InvalidRegister, Ldrb(Bn(0), Xn(1)));
        A64_REFUSES(OutOfRange, Ldr(Sn(0), Xn(1), 16384));
    }
}

// Loads and Stores: Load/store register (unscaled immediate), (immediate
// post-indexed) and (immediate pre-indexed), which share one encoding page
// and one signed nine-bit field.
// size | 111 | V | 00 | opc | 0 | imm9 | mode | Rn | Rt
A64_VECTORS("AArch64 encodes unscaled and indexed loads and stores") {
    A64_GROUP("LDUR and STUR reach either side of the base") {
        A64_ENCODES(0xF8400020, Ldur(Xn(0), Xn(1)));
        A64_ENCODES(0xF8401020, Ldur(Xn(0), Xn(1), 1));
        A64_ENCODES(0xF84FF020, Ldur(Xn(0), Xn(1), 255));
        A64_ENCODES(0xF85FF020, Ldur(Xn(0), Xn(1), -1));
        A64_ENCODES(0xF8500020, Ldur(Xn(0), Xn(1), -256));
        A64_ENCODES(0xF85EF3E0, Ldur(Xn(0), Sp, -17));
        A64_ENCODES(0xB8401024, Ldur(Wn(4), Xn(1), 1));
        A64_ENCODES(0xF81F8020, Stur(Xn(0), Xn(1), -8));
        A64_ENCODES(0x38001020, Sturb(Wn(0), Xn(1), 1));
        A64_ENCODES(0x78100020, Sturh(Wn(0), Xn(1), -256));
        A64_ENCODES(0x388FF022, Ldursb(Xn(2), Xn(1), 255));
        A64_ENCODES(0x789FF022, Ldursh(Xn(2), Xn(1), -1));
        A64_ENCODES(0xB89EF022, Ldursw(Xn(2), Xn(1), -17));
        // The SIMD widths reach the same 9 bits, unscaled at every one of them.
        A64_ENCODES(0x3CCFF025, Ldur(Qn(5), Xn(1), 255));
        // The narrowing forms reach the same nine bits, and a byte access is
        // the one width at which nothing distinguishes them from the scaled
        // page but the offsets it cannot reach.
        A64_ENCODES(0x38500020, Ldurb(Wn(0), Xn(1), -256));
        A64_ENCODES(0x38407020, Ldurb(Wn(0), Xn(1), 7));
        A64_ENCODES(0x784FF020, Ldurh(Wn(0), Xn(1), 255));
    }

    A64_GROUP("pre-index and post-index write the base back") {
        // ARM spells these two modes LDR and STR: they differ from LDUR and
        // STUR only in the two bits below the immediate.
        A64_ENCODES(0xF84FF420, Ldur(Xn(0), Xn(1), 255, A64IndexMode::PostIndex));
        A64_ENCODES(0xF84FFC20, Ldur(Xn(0), Xn(1), 255, A64IndexMode::PreIndex));
        A64_ENCODES(0xF8500420, Ldur(Xn(0), Xn(1), -256, A64IndexMode::PostIndex));
        A64_ENCODES(0xF84FFFE0, Ldur(Xn(0), Sp, 255, A64IndexMode::PreIndex));
        A64_ENCODES(0x3C900FE5, Stur(Qn(5), Sp, -256, A64IndexMode::PreIndex));
        A64_ENCODES(0x3C8FF425, Stur(Qn(5), Xn(1), 255, A64IndexMode::PostIndex));
        A64_ENCODES(0x3C500421, Ldur(Bn(1), Xn(1), -256, A64IndexMode::PostIndex));
        A64_ENCODES(0xBC4FFFE3, Ldur(Sn(3), Sp, 255, A64IndexMode::PreIndex));
    }

    A64_GROUP("refusals") {
        A64_REFUSES(OutOfRange, Ldur(Xn(0), Xn(1), 256));
        A64_REFUSES(OutOfRange, Ldur(Xn(0), Xn(1), -257));
        A64_REFUSES(OutOfRange, Stur(Xn(0), Xn(1), 256, A64IndexMode::PreIndex));
        A64_REFUSES(InvalidRegister, Ldur(Xn(0), Xzr));
        A64_REFUSES(InvalidRegister, Ldur(Sp, Xn(1)));
        A64_REFUSES(InvalidRegister, Ldurb(Xn(0), Xn(1)));
        A64_REFUSES(InvalidRegister, Ldursw(Wn(0), Xn(1)));
        A64_REFUSES(OutOfRange, Ldurh(Wn(0), Xn(1), 256));
        A64_REFUSES(InvalidRegister, Ldurh(Xn(0), Xn(1)));
    }
}

// Loads and Stores: Load/store register (register offset).
// size | 111 | V | 00 | opc | 1 | Rm | option | S | 10 | Rn | Rt
A64_VECTORS("AArch64 encodes register-offset loads and stores") {
    A64_GROUP("every option, with and without the scale bit") {
        A64_ENCODES(0xF8674820, LdrReg(Xn(0), Xn(1), Wn(7), A64ExtendKind::Uxtw));
        A64_ENCODES(0xF8675820, LdrReg(Xn(0), Xn(1), Wn(7), A64ExtendKind::Uxtw, 3));
        // UXTX is the encoding the assembly syntax spells LSL.
        A64_ENCODES(0xF8676820, LdrReg(Xn(0), Xn(1), Xn(7)));
        A64_ENCODES(0xF8677820, LdrReg(Xn(0), Xn(1), Xn(7), A64ExtendKind::Uxtx, 3));
        A64_ENCODES(0xF867C820, LdrReg(Xn(0), Xn(1), Wn(7), A64ExtendKind::Sxtw));
        A64_ENCODES(0xF867D820, LdrReg(Xn(0), Xn(1), Wn(7), A64ExtendKind::Sxtw, 3));
        A64_ENCODES(0xF867E820, LdrReg(Xn(0), Xn(1), Xn(7), A64ExtendKind::Sxtx));
        A64_ENCODES(0xF867F820, LdrReg(Xn(0), Xn(1), Xn(7), A64ExtendKind::Sxtx, 3));
        A64_ENCODES(0xB8677821, LdrReg(Wn(1), Xn(1), Xn(7), A64ExtendKind::Uxtx, 2));
        A64_ENCODES(0xF8277BE0, StrReg(Xn(0), Sp, Xn(7), A64ExtendKind::Uxtx, 3));
    }

    A64_GROUP("the scale follows the access width") {
        A64_ENCODES(0x7C677823, LdrReg(Hn(3), Xn(1), Xn(7), A64ExtendKind::Uxtx, 1));
        A64_ENCODES(0x3CE77826, LdrReg(Qn(6), Xn(1), Xn(7), A64ExtendKind::Uxtx, 4));
        A64_ENCODES(0x3CE76826, LdrReg(Qn(6), Xn(1), Xn(7)));
        A64_ENCODES(0x78675820, LdrhReg(Wn(0), Xn(1), Wn(7), A64ExtendKind::Uxtw, 1));
        A64_ENCODES(0xB8A77820, LdrswReg(Xn(0), Xn(1), Xn(7), A64ExtendKind::Uxtx, 2));
        A64_ENCODES(0x38A7C820, LdrsbReg(Xn(0), Xn(1), Wn(7), A64ExtendKind::Sxtw));
        // A byte access has nothing to scale, so it only ever writes S as zero.
        A64_ENCODES(0x38676820, LdrbReg(Wn(0), Xn(1), Xn(7)));
        A64_ENCODES(0x38274820, StrbReg(Wn(0), Xn(1), Wn(7), A64ExtendKind::Uxtw));
        A64_ENCODES(0x78277820, StrhReg(Wn(0), Xn(1), Xn(7), A64ExtendKind::Uxtx, 1));
        A64_ENCODES(0x78274820, StrhReg(Wn(0), Xn(1), Wn(7), A64ExtendKind::Uxtw));
        A64_ENCODES(0x78A7D820, LdrshReg(Xn(0), Xn(1), Wn(7), A64ExtendKind::Sxtw, 1));
        // LDRSH into a W register extends to the width the register names,
        // exactly as the unsigned-offset form does.
        A64_ENCODES(0x78E76822, LdrshReg(Wn(2), Xn(1), Xn(7)));
    }

    A64_GROUP("refusals") {
        // Only the word and doubleword extensions index memory.
        A64_REFUSES(InvalidImmediate, LdrReg(Xn(0), Xn(1), Wn(7), A64ExtendKind::Uxtb));
        A64_REFUSES(InvalidImmediate, LdrReg(Xn(0), Xn(1), Wn(7), A64ExtendKind::Sxth));
        // The extension names the width of the index.
        A64_REFUSES(InvalidRegister, LdrReg(Xn(0), Xn(1), Xn(7), A64ExtendKind::Uxtw));
        A64_REFUSES(InvalidRegister, LdrReg(Xn(0), Xn(1), Wn(7), A64ExtendKind::Uxtx));
        // The shift is a scale bit, so it is either absent or exactly the log2
        // of the access width.
        A64_REFUSES(InvalidImmediate, LdrReg(Xn(0), Xn(1), Xn(7), A64ExtendKind::Uxtx, 2));
        A64_REFUSES(InvalidImmediate, LdrbReg(Wn(0), Xn(1), Xn(7), A64ExtendKind::Uxtx, 1));
        A64_REFUSES(InvalidRegister, LdrReg(Xn(0), Xzr, Xn(7)));
        A64_REFUSES(InvalidRegister, LdrReg(Xn(0), Xn(1), Sp));
        A64_REFUSES(InvalidImmediate, LdrshReg(Xn(0), Xn(1), Xn(7), A64ExtendKind::Uxtx, 2));
        A64_REFUSES(InvalidRegister, StrhReg(Xn(0), Xn(1), Xn(7)));
    }
}

// Loads and Stores: Load/store register pair (offset), (post-indexed) and
// (pre-indexed), which differ only in the three bits naming the mode.
// opc | 101 | V | mode | L | imm7 | Rt2 | Rn | Rt
A64_VECTORS("AArch64 encodes load and store pair") {
    A64_GROUP("general purpose, scaled by one register") {
        A64_ENCODES(0xA940783D, Ldp(Fp, Lr, Xn(1)));
        A64_ENCODES(0xA940F83D, Ldp(Fp, Lr, Xn(1), 8));
        A64_ENCODES(0xA95FF83D, Ldp(Fp, Lr, Xn(1), 504));
        A64_ENCODES(0xA960783D, Ldp(Fp, Lr, Xn(1), -512));
        A64_ENCODES(0xA93F783D, Stp(Fp, Lr, Xn(1), -16));
        A64_ENCODES(0xA97F7FE0, Ldp(Xn(0), Xzr, Sp, -16));
        A64_ENCODES(0x29409023, Ldp(Wn(3), Wn(4), Xn(1), 4));
        A64_ENCODES(0x29201023, Stp(Wn(3), Wn(4), Xn(1), -256));
        A64_ENCODES(0x295F9023, Ldp(Wn(3), Wn(4), Xn(1), 252));
    }

    A64_GROUP("the frame chain a prologue and epilogue move") {
        A64_ENCODES(0xA9BF7BFD, Stp(Fp, Lr, Sp, -16, A64IndexMode::PreIndex));
        A64_ENCODES(0xA8C17BFD, Ldp(Fp, Lr, Sp, 16, A64IndexMode::PostIndex));
        A64_ENCODES(0xA9A07BFD, Stp(Fp, Lr, Sp, -512, A64IndexMode::PreIndex));
        A64_ENCODES(0xA8DFFBFD, Ldp(Fp, Lr, Sp, 504, A64IndexMode::PostIndex));
    }

    A64_GROUP("SIMD") {
        A64_ENCODES(0x2D408420, Ldp(Sn(0), Sn(1), Xn(1), 4));
        A64_ENCODES(0x2D200420, Stp(Sn(0), Sn(1), Xn(1), -256));
        A64_ENCODES(0x6D40A428, Ldp(Dn(8), Dn(9), Xn(1), 8));
        A64_ENCODES(0x6DA02428, Stp(Dn(8), Dn(9), Xn(1), -512, A64IndexMode::PreIndex));
        A64_ENCODES(0xAD600C22, Ldp(Qn(2), Qn(3), Xn(1), -1024));
        A64_ENCODES(0xACBF0C22, Stp(Qn(2), Qn(3), Xn(1), -32, A64IndexMode::PostIndex));
        // The far end of the field at each pair width.
        A64_ENCODES(0x2D5F8420, Ldp(Sn(0), Sn(1), Xn(1), 252));
        A64_ENCODES(0x6D5FA428, Ldp(Dn(8), Dn(9), Xn(1), 504));
        A64_ENCODES(0xAD5F8C22, Ldp(Qn(2), Qn(3), Xn(1), 1008));
    }

    A64_GROUP("refusals") {
        A64_REFUSES(Unaligned, Ldp(Xn(0), Xn(1), Xn(2), 4));
        A64_REFUSES(OutOfRange, Ldp(Xn(0), Xn(1), Xn(2), 512));
        A64_REFUSES(OutOfRange, Ldp(Xn(0), Xn(1), Xn(2), -520));
        // The two registers are one width and one file.
        A64_REFUSES(InvalidRegister, Ldp(Xn(0), Wn(1), Xn(2)));
        A64_REFUSES(InvalidRegister, Ldp(Xn(0), Dn(1), Xn(2)));
        // There is no byte or halfword pair, and neither reading of code 31 is
        // a transferable register.
        A64_REFUSES(InvalidRegister, Ldp(Bn(0), Bn(1), Xn(2)));
        A64_REFUSES(InvalidRegister, Ldp(Hn(0), Hn(1), Xn(2)));
        A64_REFUSES(InvalidRegister, Stp(Sp, Xn(1), Xn(2)));
        A64_REFUSES(InvalidRegister, Stp(Xn(0), Xn(1), Xzr));
        A64_REFUSES(OutOfRange, Ldp(Qn(0), Qn(1), Xn(2), 1024));
    }
}

// Loads and Stores: Load register (literal).
// opc | 011 | V | 00 | imm19 | Rt
A64_VECTORS("AArch64 encodes LDR from a PC-relative literal") {
    A64_ENCODES(0x58000000, LdrLiteral(Xn(0), 0));
    A64_ENCODES(0x58000020, LdrLiteral(Xn(0), 4));
    A64_ENCODES(0x58FFFFE0, LdrLiteral(Xn(0), -4));
    A64_ENCODES(0x18000023, LdrLiteral(Wn(3), 4));
    A64_ENCODES(0x1C000005, LdrLiteral(Sn(5), 0));
    A64_ENCODES(0x5C000007, LdrLiteral(Dn(7), 0));
    A64_ENCODES(0x9C000009, LdrLiteral(Qn(9), 0));
    // The boundaries of the 19-bit field, counted in instructions.
    A64_ENCODES(0x587FFFE0, LdrLiteral(Xn(0), 1048572));
    A64_ENCODES(0x58800000, LdrLiteral(Xn(0), -1048576));

    A64_REFUSES(Unaligned, LdrLiteral(Xn(0), 2));
    A64_REFUSES(OutOfRange, LdrLiteral(Xn(0), 1048576));
    A64_REFUSES(OutOfRange, LdrLiteral(Xn(0), -1048580));
    A64_REFUSES(InvalidRegister, LdrLiteral(Bn(0), 0));
    A64_REFUSES(InvalidRegister, LdrLiteral(Hn(0), 0));
    A64_REFUSES(InvalidRegister, LdrLiteral(Sp, 0));
}

// Branches, Exception Generating and System instructions: Unconditional
// branch (immediate) and Conditional branch (immediate).
// op | 00101 | imm26
// 0101010 | o1 | imm19 | o0 | cond
A64_VECTORS("AArch64 encodes immediate branches") {
    A64_GROUP("B and BL over the 26-bit field") {
        A64_ENCODES(0x14000000, B(0));
        A64_ENCODES(0x14000001, B(4));
        A64_ENCODES(0x17FFFFFF, B(-4));
        A64_ENCODES(0x14000100, B(1024));
        A64_ENCODES(0x17FFFF00, B(-1024));
        A64_ENCODES(0x94000000, Bl(0));
        A64_ENCODES(0x94000001, Bl(4));
        A64_ENCODES(0x97FFFFFF, Bl(-4));
        // +/-128 MiB, the whole reach of a call needing no veneer.
        A64_ENCODES(0x15FFFFFF, B(134217724));
        A64_ENCODES(0x16000000, B(-134217728));
        A64_ENCODES(0x95FFFFFF, Bl(134217724));
        A64_ENCODES(0x96000000, Bl(-134217728));
    }

    A64_GROUP("B.cond at every condition") {
        A64_ENCODES(0x54000040, BCond(A64Condition::Eq, 8));
        A64_ENCODES(0x54000041, BCond(A64Condition::Ne, 8));
        A64_ENCODES(0x54000042, BCond(A64Condition::Cs, 8));
        A64_ENCODES(0x54000043, BCond(A64Condition::Cc, 8));
        A64_ENCODES(0x54000044, BCond(A64Condition::Mi, 8));
        A64_ENCODES(0x54000045, BCond(A64Condition::Pl, 8));
        A64_ENCODES(0x54000046, BCond(A64Condition::Vs, 8));
        A64_ENCODES(0x54000047, BCond(A64Condition::Vc, 8));
        A64_ENCODES(0x54000048, BCond(A64Condition::Hi, 8));
        A64_ENCODES(0x54000049, BCond(A64Condition::Ls, 8));
        A64_ENCODES(0x5400004A, BCond(A64Condition::Ge, 8));
        A64_ENCODES(0x5400004B, BCond(A64Condition::Lt, 8));
        A64_ENCODES(0x5400004C, BCond(A64Condition::Gt, 8));
        A64_ENCODES(0x5400004D, BCond(A64Condition::Le, 8));
        // Neither of the unconditional codes is being inverted here, so both
        // encode as written rather than being refused as they are in CSET.
        A64_ENCODES(0x5400004E, BCond(A64Condition::Al, 8));
        A64_ENCODES(0x5400004F, BCond(A64Condition::Nv, 8));
        // The boundaries of the 19-bit field: +/-1 MiB.
        A64_ENCODES(0x547FFFE0, BCond(A64Condition::Eq, 1048572));
        A64_ENCODES(0x54800000, BCond(A64Condition::Eq, -1048576));
    }

    A64_GROUP("refusals") {
        // An offset that names no instruction is not rounded to one, and one
        // past the end of the field does not wrap into a branch elsewhere.
        A64_REFUSES(Unaligned, B(2));
        A64_REFUSES(Unaligned, Bl(-1));
        A64_REFUSES(Unaligned, BCond(A64Condition::Eq, 6));
        A64_REFUSES(OutOfRange, B(134217728));
        A64_REFUSES(OutOfRange, B(-134217732));
        A64_REFUSES(OutOfRange, Bl(134217728));
        A64_REFUSES(OutOfRange, BCond(A64Condition::Ne, 1048576));
        A64_REFUSES(OutOfRange, BCond(A64Condition::Ne, -1048580));
    }
}

// Branches, Exception Generating and System instructions: Compare and branch
// (immediate) and Test and branch (immediate).
// sf | 011010 | op | imm19 | Rt
// b5 | 011011 | op | b40 | imm14 | Rt
A64_VECTORS("AArch64 encodes compare-and-branch and test-and-branch") {
    A64_GROUP("CBZ and CBNZ") {
        A64_ENCODES(0xB4000080, Cbz(Xn(0), 16));
        A64_ENCODES(0xB4000093, Cbz(Xn(19), 16));
        A64_ENCODES(0x35000080, Cbnz(Wn(0), 16));
        A64_ENCODES(0x3500009E, Cbnz(Wn(30), 16));
        A64_ENCODES(0x3400009F, Cbz(Wzr, 16));
        A64_ENCODES(0xB47FFFE0, Cbz(Xn(0), 1048572));
        A64_ENCODES(0xB5800000, Cbnz(Xn(0), -1048576));
    }

    A64_GROUP("TBZ and TBNZ split the bit index across two fields") {
        A64_ENCODES(0x36000063, Tbz(Xn(3), 0, 12));
        A64_ENCODES(0x36080063, Tbz(Xn(3), 1, 12));
        A64_ENCODES(0x37000063, Tbnz(Xn(3), 0, 12));
        // Bit 31 is the last one b40 holds alone; bit 32 is the first to set b5,
        // which sits where every other instruction keeps sf.
        A64_ENCODES(0x36F80063, Tbz(Xn(3), 31, 12));
        A64_ENCODES(0xB6000063, Tbz(Xn(3), 32, 12));
        A64_ENCODES(0xB6F80063, Tbz(Xn(3), 63, 12));
        A64_ENCODES(0x37F80063, Tbnz(Xn(3), 31, 12));
        A64_ENCODES(0xB7000063, Tbnz(Xn(3), 32, 12));
        A64_ENCODES(0xB7F80063, Tbnz(Xn(3), 63, 12));
        // A W register reaches only the low half, and encodes identically to
        // the X register it is a view of.
        A64_ENCODES(0x3607FFA5, Tbz(Wn(5), 0, -12));
        A64_ENCODES(0x36FFFFA5, Tbz(Wn(5), 31, -12));
        // The boundaries of the 14-bit field: +/-32 KiB.
        A64_ENCODES(0x3603FFE0, Tbz(Xn(0), 0, 32764));
        A64_ENCODES(0xB7FC0000, Tbnz(Xn(0), 63, -32768));
    }

    A64_GROUP("refusals") {
        A64_REFUSES(Unaligned, Cbz(Xn(0), 2));
        A64_REFUSES(OutOfRange, Cbz(Xn(0), 1048576));
        A64_REFUSES(OutOfRange, Cbnz(Xn(0), -1048580));
        A64_REFUSES(Unaligned, Tbz(Xn(0), 0, 2));
        A64_REFUSES(OutOfRange, Tbz(Xn(0), 0, 32768));
        A64_REFUSES(OutOfRange, Tbnz(Xn(0), 0, -32772));
        // The bit index has to name a bit the register has.
        A64_REFUSES(InvalidImmediate, Tbz(Wn(0), 32, 12));
        A64_REFUSES(InvalidImmediate, Tbz(Xn(0), 64, 12));
        // Neither form compares the stack pointer or anything but a general
        // register.
        A64_REFUSES(InvalidRegister, Cbz(Sp, 16));
        A64_REFUSES(InvalidRegister, Cbz(Dn(0), 16));
        A64_REFUSES(InvalidRegister, Tbz(Wsp, 0, 12));
        A64_REFUSES(InvalidRegister, Tbnz(Qn(0), 0, 12));
    }
}

// Branches, Exception Generating and System instructions: Unconditional
// branch (register).
// 1101011 | opc | op2 | op3 | Rn | op4
A64_VECTORS("AArch64 encodes branches through a register") {
    A64_ENCODES(0xD61F0000, Br(Xn(0)));
    A64_ENCODES(0xD61F0200, Br(Ip0));
    A64_ENCODES(0xD61F03C0, Br(Lr));
    A64_ENCODES(0xD61F03E0, Br(Xzr));
    A64_ENCODES(0xD63F0000, Blr(Xn(0)));
    A64_ENCODES(0xD63F0200, Blr(Ip0));
    A64_ENCODES(0xD63F03C0, Blr(Lr));
    A64_ENCODES(0xD65F0000, Ret(Xn(0)));
    A64_ENCODES(0xD65F03C0, Ret(Lr));
    // An epilogue returns through the link register, so that is what RET is
    // written without an operand.
    A64_ENCODES(0xD65F03C0, Ret());

    // The target is an address, so it is 64-bit and never the stack pointer.
    A64_REFUSES(InvalidRegister, Br(Wn(0)));
    A64_REFUSES(InvalidRegister, Blr(Sp));
    A64_REFUSES(InvalidRegister, Ret(Dn(0)));
}

// Branches, Exception Generating and System instructions: Exception
// generation.
// 11010100 | opc | imm16 | op2 | LL
A64_VECTORS("AArch64 encodes exception generation") {
    A64_ENCODES(0xD4000001, Svc(0));
    A64_ENCODES(0xD4000021, Svc(1));
    // The syscall wrappers issue SVC #0; the rest of the field is there for a
    // debugger rather than for the kernel.
    A64_ENCODES(0xD4024681, Svc(0x1234));
    A64_ENCODES(0xD41FFFE1, Svc(0xFFFF));
    A64_ENCODES(0xD4200000, Brk(0));
    A64_ENCODES(0xD4200020, Brk(1));
    A64_ENCODES(0xD4224680, Brk(0x1234));
    A64_ENCODES(0xD43FFFE0, Brk(0xFFFF));
    A64_ENCODES(0xD4400000, Hlt(0));
    A64_ENCODES(0xD4424680, Hlt(0x1234));
    A64_ENCODES(0xD45FFFE0, Hlt(0xFFFF));

    // UDF is a hole in the instruction space rather than an instruction, so its
    // word is the immediate and nothing else — which is why a jump into zeroed
    // memory traps on the first word it reaches.
    A64_ENCODES(0x00000000, Udf(0));
    A64_ENCODES(0x00000001, Udf(1));
    A64_ENCODES(0x0000FFFF, Udf(0xFFFF));
}

// Branches, Exception Generating and System instructions: Hints and
// Barriers, both pages of the System instruction group.
// 1101010100 | 0 | 00 | 011 | CRn | CRm | op2 | 11111
A64_VECTORS("AArch64 encodes hints and barriers") {
    A64_GROUP("hints") {
        A64_ENCODES(0xD503201F, Nop());
        A64_ENCODES(0xD503201F, Hint(0));
        A64_ENCODES(0xD503203F, Hint(1));
        A64_ENCODES(0xD503207F, Hint(3));
        // The immediate is CRm and op2 together, so it carries into the field
        // above at every multiple of eight.
        A64_ENCODES(0xD503211F, Hint(8));
        A64_ENCODES(0xD5032FFF, Hint(127));
        A64_REFUSES(InvalidImmediate, Hint(128));
    }

    A64_GROUP("barriers at every named option") {
        A64_ENCODES(0xD5033FBF, Dmb());
        A64_ENCODES(0xD50331BF, Dmb(A64Barrier::Oshld));
        A64_ENCODES(0xD50332BF, Dmb(A64Barrier::Oshst));
        A64_ENCODES(0xD50333BF, Dmb(A64Barrier::Osh));
        A64_ENCODES(0xD50335BF, Dmb(A64Barrier::Nshld));
        A64_ENCODES(0xD50336BF, Dmb(A64Barrier::Nshst));
        A64_ENCODES(0xD50337BF, Dmb(A64Barrier::Nsh));
        A64_ENCODES(0xD50339BF, Dmb(A64Barrier::Ishld));
        A64_ENCODES(0xD5033ABF, Dmb(A64Barrier::Ishst));
        A64_ENCODES(0xD5033BBF, Dmb(A64Barrier::Ish));
        A64_ENCODES(0xD5033DBF, Dmb(A64Barrier::Ld));
        A64_ENCODES(0xD5033EBF, Dmb(A64Barrier::St));
        A64_ENCODES(0xD5033FBF, Dmb(A64Barrier::Sy));

        // DSB differs from DMB in op2 alone, so the option field encodes the
        // same way in both.
        A64_ENCODES(0xD5033F9F, Dsb());
        A64_ENCODES(0xD503339F, Dsb(A64Barrier::Osh));
        A64_ENCODES(0xD5033B9F, Dsb(A64Barrier::Ish));
        A64_ENCODES(0xD5033D9F, Dsb(A64Barrier::Ld));
        A64_ENCODES(0xD5033F9F, Dsb(A64Barrier::Sy));

        // ISB has one meaningful option, and it is the default.
        A64_ENCODES(0xD5033FDF, Isb());
        A64_ENCODES(0xD5033FDF, Isb(A64Barrier::Sy));
    }
}

// Branches, Exception Generating and System instructions: System register
// move, whose system-register field is the encoding D19.2 tabulates.
// 1101010100 | L | 1 | o0 | op1 | CRn | CRm | op2 | Rt
A64_VECTORS("AArch64 encodes moves to and from a system register") {
    A64_ENCODES(0xD53B4200, Mrs(Xn(0), Nzcv));
    A64_ENCODES(0xD53B4201, Mrs(Xn(1), Nzcv));
    A64_ENCODES(0xD53B421E, Mrs(Lr, Nzcv));
    A64_ENCODES(0xD53B421F, Mrs(Xzr, Nzcv));
    A64_ENCODES(0xD51B4200, Msr(Nzcv, Xn(0)));
    A64_ENCODES(0xD51B421E, Msr(Nzcv, Lr));

    A64_ENCODES(0xD53BD040, Mrs(Xn(0), TpidrEl0));
    A64_ENCODES(0xD53BD05E, Mrs(Lr, TpidrEl0));
    A64_ENCODES(0xD51BD040, Msr(TpidrEl0, Xn(0)));

    // Any register the 15-bit encoding names is reachable, not only the two
    // that have a constant.
    A64_ENCODES(0xD53B4401, Mrs(Xn(1), SysReg(3, 3, 4, 4, 0))); // FPCR
    A64_ENCODES(0xD53B4421, Mrs(Xn(1), SysReg(3, 3, 4, 4, 1))); // FPSR
    A64_ENCODES(0xD51B4402, Msr(SysReg(3, 3, 4, 4, 0), Xn(2))); // FPCR
    A64_ENCODES(0xD5380001, Mrs(Xn(1), SysReg(3, 0, 0, 0, 0))); // MIDR_EL1
    A64_ENCODES(0xD53B0021, Mrs(Xn(1), SysReg(3, 3, 0, 0, 1))); // CTR_EL0

    // The transferred register is 64-bit whatever the width of the system
    // register, and code 31 is the zero register there.
    A64_REFUSES(InvalidRegister, Mrs(Wn(0), Nzcv));
    A64_REFUSES(InvalidRegister, Mrs(Sp, Nzcv));
    A64_REFUSES(InvalidRegister, Msr(Nzcv, Wn(0)));
    A64_REFUSES(InvalidRegister, Msr(Nzcv, Dn(0)));
}

// Data Processing -- Scalar Floating-Point: Floating-point immediate, the
// FMOV of Floating-point data-processing (1 source), and the four FMOV
// directions of Conversion between floating-point and integer.
// M | 0 | S | 11110 | ptype | 1 | imm8 | 100 | imm5 | Rd
// M | 0 | S | 11110 | ptype | 1 | opcode | 10000 | Rn | Rd
// sf | 0 | S | 11110 | ptype | 1 | rmode | opcode | 000000 | Rn | Rd
A64_VECTORS("AArch64 encodes floating-point moves") {
    A64_GROUP("register to register at both precisions") {
        A64_ENCODES(0x1E204020, Fmov(Sn(0), Sn(1)));
        A64_ENCODES(0x1E2043DF, Fmov(Sn(31), Sn(30)));
        A64_ENCODES(0x1E604020, Fmov(Dn(0), Dn(1)));
        A64_ENCODES(0x1E6043DF, Fmov(Dn(31), Dn(30)));
        A64_ENCODES(0x1E60418C, Fmov(Dn(12), Dn(12)));
    }

    A64_GROUP("all four transfers to and from the general-purpose file") {
        // The bits of the value move unchanged, so a word pairs with an S register
        // and a doubleword with a D one.
        A64_ENCODES(0x1E260020, Fmov(Wn(0), Sn(1)));
        A64_ENCODES(0x1E2603FE, Fmov(Wn(30), Sn(31)));
        A64_ENCODES(0x1E270020, Fmov(Sn(0), Wn(1)));
        A64_ENCODES(0x1E2703DF, Fmov(Sn(31), Wn(30)));
        A64_ENCODES(0x9E660020, Fmov(Xn(0), Dn(1)));
        A64_ENCODES(0x9E6603FE, Fmov(Xn(30), Dn(31)));
        A64_ENCODES(0x9E670020, Fmov(Dn(0), Xn(1)));
        A64_ENCODES(0x9E6703DF, Fmov(Dn(31), Xn(30)));
        A64_ENCODES(0x9E66009F, Fmov(Xzr, Dn(4)));

        // A pairing of different widths, a move between two general-purpose
        // registers, and the stack pointer all name no encoding here.
        A64_REFUSES(InvalidRegister, Fmov(Xn(0), Sn(1)));
        A64_REFUSES(InvalidRegister, Fmov(Wn(0), Dn(1)));
        A64_REFUSES(InvalidRegister, Fmov(Dn(0), Wn(1)));
        A64_REFUSES(InvalidRegister, Fmov(Xn(0), Xn(1)));
        A64_REFUSES(InvalidRegister, Fmov(Sp, Dn(0)));
        A64_REFUSES(InvalidRegister, Fmov(Sn(0), Dn(1)));
        A64_REFUSES(InvalidRegister, Fmov(Bn(0), Bn(1)));
        A64_REFUSES(InvalidRegister, Fmov(Hn(0), Hn(1)));
        A64_REFUSES(InvalidRegister, Fmov(Qn(0), Qn(1)));
    }

    A64_GROUP("the eight binades the immediate reaches") {
        A64_ENCODES(0x1E6E1000, FmovImm(Dn(0), 1.0));
        A64_ENCODES(0x1E601000, FmovImm(Dn(0), 2.0));
        A64_ENCODES(0x1E7E1000, FmovImm(Dn(0), -1.0));
        A64_ENCODES(0x1E6C1003, FmovImm(Dn(3), 0.5));
        A64_ENCODES(0x1E67F01F, FmovImm(Dn(31), 31.0));
        A64_ENCODES(0x1E681000, FmovImm(Dn(0), 0.125));
        A64_ENCODES(0x1E781000, FmovImm(Dn(0), -0.125));
        A64_ENCODES(0x1E6FF000, FmovImm(Dn(0), 1.9375));
        A64_ENCODES(0x1E2E1000, FmovImm(Sn(0), 1.0));
        A64_ENCODES(0x1E309011, FmovImm(Sn(17), -2.5));
        A64_ENCODES(0x1E27F01F, FmovImm(Sn(31), 31.0));
        A64_ENCODES(0x1E281000, FmovImm(Sn(0), 0.125));
        // The immediate field full, which is the largest magnitude it reaches
        // with the sign bit set.
        A64_ENCODES(0x1E77F000, FmovImm(Dn(0), -31.0));
        A64_ENCODES(0x1E37F000, FmovImm(Sn(0), -31.0));

        // Zero has no encoding, since the immediate always carries the leading one
        // of a normal number; nor has anything outside the eight binades, or a
        // fraction needing more than four bits. Those come from the literal pool.
        A64_REFUSES(InvalidImmediate, FmovImm(Dn(0), 0.0));
        A64_REFUSES(InvalidImmediate, FmovImm(Dn(0), -0.0));
        A64_REFUSES(InvalidImmediate, FmovImm(Dn(0), 32.0));
        A64_REFUSES(InvalidImmediate, FmovImm(Dn(0), 0.0625));
        A64_REFUSES(InvalidImmediate, FmovImm(Dn(0), 0.1));
        A64_REFUSES(InvalidImmediate, FmovImm(Dn(0), 3.14159));
        A64_REFUSES(InvalidRegister, FmovImm(Xn(0), 1.0));
        A64_REFUSES(InvalidRegister, FmovImm(Qn(0), 1.0));
    }
}

// Data Processing -- Scalar Floating-Point: Floating-point data-processing
// (2 source) and (1 source).
// M | 0 | S | 11110 | ptype | 1 | Rm | opcode | 10 | Rn | Rd
// M | 0 | S | 11110 | ptype | 1 | opcode | 10000 | Rn | Rd
A64_VECTORS("AArch64 encodes floating-point arithmetic") {
    A64_GROUP("the two-source group at both precisions") {
        A64_ENCODES(0x1E222820, Fadd(Sn(0), Sn(1), Sn(2)));
        A64_ENCODES(0x1E622820, Fadd(Dn(0), Dn(1), Dn(2)));
        A64_ENCODES(0x1E223820, Fsub(Sn(0), Sn(1), Sn(2)));
        A64_ENCODES(0x1E623820, Fsub(Dn(0), Dn(1), Dn(2)));
        A64_ENCODES(0x1E220820, Fmul(Sn(0), Sn(1), Sn(2)));
        A64_ENCODES(0x1E620820, Fmul(Dn(0), Dn(1), Dn(2)));
        A64_ENCODES(0x1E221820, Fdiv(Sn(0), Sn(1), Sn(2)));
        A64_ENCODES(0x1E621820, Fdiv(Dn(0), Dn(1), Dn(2)));

        // Every register field reaches the whole file.
        A64_ENCODES(0x1E60281F, Fadd(Dn(31), Dn(0), Dn(0)));
        A64_ENCODES(0x1E602BE0, Fadd(Dn(0), Dn(31), Dn(0)));
        A64_ENCODES(0x1E7F2800, Fadd(Dn(0), Dn(0), Dn(31)));
        A64_ENCODES(0x1E2E3935, Fsub(Sn(21), Sn(9), Sn(14)));
    }

    A64_GROUP("the one-source group") {
        A64_ENCODES(0x1E214020, Fneg(Sn(0), Sn(1)));
        A64_ENCODES(0x1E614020, Fneg(Dn(0), Dn(1)));
        A64_ENCODES(0x1E20C020, Fabs(Sn(0), Sn(1)));
        A64_ENCODES(0x1E60C020, Fabs(Dn(0), Dn(1)));
        A64_ENCODES(0x1E21C020, Fsqrt(Sn(0), Sn(1)));
        A64_ENCODES(0x1E61C020, Fsqrt(Dn(0), Dn(1)));
        A64_ENCODES(0x1E6143FF, Fneg(Dn(31), Dn(31)));
        A64_ENCODES(0x1E20C230, Fabs(Sn(16), Sn(17)));
    }

    A64_GROUP("operands that disagree on precision") {
        // Every operand of one instruction carries the width its destination does,
        // and half precision has no form at all here.
        A64_REFUSES(InvalidRegister, Fadd(Sn(0), Dn(1), Sn(2)));
        A64_REFUSES(InvalidRegister, Fadd(Sn(0), Sn(1), Dn(2)));
        A64_REFUSES(InvalidRegister, Fmul(Dn(0), Sn(1), Dn(2)));
        A64_REFUSES(InvalidRegister, Fdiv(Hn(0), Hn(1), Hn(2)));
        A64_REFUSES(InvalidRegister, Fadd(Xn(0), Xn(1), Xn(2)));
        A64_REFUSES(InvalidRegister, Fneg(Sn(0), Dn(1)));
        A64_REFUSES(InvalidRegister, Fsqrt(Qn(0), Qn(1)));
        A64_REFUSES(InvalidRegister, Fabs(Wn(0), Sn(1)));
    }
}

// Data Processing -- Scalar Floating-Point: Floating-point data-processing
// (3 source) and (2 source).
// M | 0 | S | 11111 | ptype | o1 | Rm | o0 | Ra | Rn | Rd
A64_VECTORS("AArch64 encodes fused multiply-add and the min and max group") {
    A64_GROUP("the three-source group") {
        A64_ENCODES(0x1F020C20, Fmadd(Sn(0), Sn(1), Sn(2), Sn(3)));
        A64_ENCODES(0x1F420C20, Fmadd(Dn(0), Dn(1), Dn(2), Dn(3)));
        A64_ENCODES(0x1F028C20, Fmsub(Sn(0), Sn(1), Sn(2), Sn(3)));
        A64_ENCODES(0x1F428C20, Fmsub(Dn(0), Dn(1), Dn(2), Dn(3)));
        A64_ENCODES(0x1F220C20, Fnmadd(Sn(0), Sn(1), Sn(2), Sn(3)));
        A64_ENCODES(0x1F620C20, Fnmadd(Dn(0), Dn(1), Dn(2), Dn(3)));
        A64_ENCODES(0x1F228C20, Fnmsub(Sn(0), Sn(1), Sn(2), Sn(3)));
        A64_ENCODES(0x1F628C20, Fnmsub(Dn(0), Dn(1), Dn(2), Dn(3)));

        // The accumulator is a field of its own, so it reaches the whole file too.
        A64_ENCODES(0x1F407C00, Fmadd(Dn(0), Dn(0), Dn(0), Dn(31)));
        A64_ENCODES(0x1F3DF3DF, Fnmsub(Sn(31), Sn(30), Sn(29), Sn(28)));

        A64_REFUSES(InvalidRegister, Fmadd(Dn(0), Dn(1), Dn(2), Sn(3)));
        A64_REFUSES(InvalidRegister, Fmsub(Sn(0), Sn(1), Sn(2), Xn(3)));
    }

    A64_GROUP("maximum and minimum, propagating and number") {
        A64_ENCODES(0x1E224820, Fmax(Sn(0), Sn(1), Sn(2)));
        A64_ENCODES(0x1E624820, Fmax(Dn(0), Dn(1), Dn(2)));
        A64_ENCODES(0x1E225820, Fmin(Sn(0), Sn(1), Sn(2)));
        A64_ENCODES(0x1E625820, Fmin(Dn(0), Dn(1), Dn(2)));
        A64_ENCODES(0x1E226820, Fmaxnm(Sn(0), Sn(1), Sn(2)));
        A64_ENCODES(0x1E626820, Fmaxnm(Dn(0), Dn(1), Dn(2)));
        A64_ENCODES(0x1E227820, Fminnm(Sn(0), Sn(1), Sn(2)));
        A64_ENCODES(0x1E627820, Fminnm(Dn(0), Dn(1), Dn(2)));
        A64_ENCODES(0x1E7F4BFF, Fmax(Dn(31), Dn(31), Dn(31)));

        A64_REFUSES(InvalidRegister, Fminnm(Dn(0), Dn(1), Sn(2)));
    }
}

// Data Processing -- Scalar Floating-Point: Floating-point compare,
// Floating-point conditional compare and Floating-point conditional select.
// M | 0 | S | 11110 | ptype | 1 | Rm | op | 1000 | Rn | opcode2
// M | 0 | S | 11110 | ptype | 1 | Rm | cond | 01 | Rn | op | nzcv
// M | 0 | S | 11110 | ptype | 1 | Rm | cond | 11 | Rn | Rd
A64_VECTORS("AArch64 encodes floating-point comparison and selection") {
    A64_GROUP("comparison, with and without a second operand") {
        A64_ENCODES(0x1E212000, Fcmp(Sn(0), Sn(1)));
        A64_ENCODES(0x1E612000, Fcmp(Dn(0), Dn(1)));
        A64_ENCODES(0x1E7E23E0, Fcmp(Dn(31), Dn(30)));
        A64_ENCODES(0x1E212010, Fcmpe(Sn(0), Sn(1)));
        A64_ENCODES(0x1E612010, Fcmpe(Dn(0), Dn(1)));

        // The zero forms compare against +0.0, which is part of the instruction
        // rather than an operand, so they leave the second register field empty.
        A64_ENCODES(0x1E202008, FcmpZero(Sn(0)));
        A64_ENCODES(0x1E602008, FcmpZero(Dn(0)));
        A64_ENCODES(0x1E6023E8, FcmpZero(Dn(31)));
        A64_ENCODES(0x1E202018, FcmpeZero(Sn(0)));
        A64_ENCODES(0x1E602238, FcmpeZero(Dn(17)));
        A64_REFUSES(InvalidRegister, Fcmp(Sn(0), Dn(1)));
        A64_REFUSES(InvalidRegister, Fcmp(Xn(0), Xn(1)));
        A64_REFUSES(InvalidRegister, FcmpZero(Hn(0)));
    }

    A64_GROUP("conditional compare at every condition") {
        A64_ENCODES(0x1E610400, Fccmp(Dn(0), Dn(1), 0, A64Condition::Eq));
        A64_ENCODES(0x1E611401, Fccmp(Dn(0), Dn(1), 1, A64Condition::Ne));
        A64_ENCODES(0x1E612402, Fccmp(Dn(0), Dn(1), 2, A64Condition::Cs));
        A64_ENCODES(0x1E613403, Fccmp(Dn(0), Dn(1), 3, A64Condition::Cc));
        A64_ENCODES(0x1E614404, Fccmp(Dn(0), Dn(1), 4, A64Condition::Mi));
        A64_ENCODES(0x1E615405, Fccmp(Dn(0), Dn(1), 5, A64Condition::Pl));
        A64_ENCODES(0x1E616406, Fccmp(Dn(0), Dn(1), 6, A64Condition::Vs));
        A64_ENCODES(0x1E617407, Fccmp(Dn(0), Dn(1), 7, A64Condition::Vc));
        A64_ENCODES(0x1E618408, Fccmp(Dn(0), Dn(1), 8, A64Condition::Hi));
        A64_ENCODES(0x1E619409, Fccmp(Dn(0), Dn(1), 9, A64Condition::Ls));
        A64_ENCODES(0x1E61A40A, Fccmp(Dn(0), Dn(1), 10, A64Condition::Ge));
        A64_ENCODES(0x1E61B40B, Fccmp(Dn(0), Dn(1), 11, A64Condition::Lt));
        A64_ENCODES(0x1E61C40C, Fccmp(Dn(0), Dn(1), 12, A64Condition::Gt));
        A64_ENCODES(0x1E61D40D, Fccmp(Dn(0), Dn(1), 13, A64Condition::Le));
        A64_ENCODES(0x1E61E40E, Fccmp(Dn(0), Dn(1), 14, A64Condition::Al));
        A64_ENCODES(0x1E61F40F, Fccmp(Dn(0), Dn(1), 15, A64Condition::Nv));
        A64_ENCODES(0x1E21040F, Fccmp(Sn(0), Sn(1), 15, A64Condition::Eq));
        A64_ENCODES(0x1E3E17E0, Fccmp(Sn(31), Sn(30), 0, A64Condition::Ne));

        // The flags written when the condition fails are four bits and no more.
        A64_REFUSES(InvalidImmediate, Fccmp(Dn(0), Dn(1), 16, A64Condition::Eq));
        A64_REFUSES(InvalidRegister, Fccmp(Dn(0), Sn(1), 0, A64Condition::Eq));
    }

    A64_GROUP("conditional select at every condition") {
        A64_ENCODES(0x1E620C20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Eq));
        A64_ENCODES(0x1E621C20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Ne));
        A64_ENCODES(0x1E622C20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Cs));
        A64_ENCODES(0x1E623C20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Cc));
        A64_ENCODES(0x1E624C20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Mi));
        A64_ENCODES(0x1E625C20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Pl));
        A64_ENCODES(0x1E626C20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Vs));
        A64_ENCODES(0x1E627C20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Vc));
        A64_ENCODES(0x1E628C20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Hi));
        A64_ENCODES(0x1E629C20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Ls));
        A64_ENCODES(0x1E62AC20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Ge));
        A64_ENCODES(0x1E62BC20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Lt));
        A64_ENCODES(0x1E62CC20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Gt));
        A64_ENCODES(0x1E62DC20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Le));
        // AL and NV are accepted as written, unlike in the general-purpose
        // conditional-select aliases: nothing is inverted here, so both
        // spellings of always mean what they say.
        A64_ENCODES(0x1E62EC20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Al));
        A64_ENCODES(0x1E62FC20, Fcsel(Dn(0), Dn(1), Dn(2), A64Condition::Nv));
        A64_ENCODES(0x1E22CC20, Fcsel(Sn(0), Sn(1), Sn(2), A64Condition::Gt));
        A64_ENCODES(0x1E3D3FDF, Fcsel(Sn(31), Sn(30), Sn(29), Lo));
        A64_REFUSES(InvalidRegister, Fcsel(Dn(0), Dn(1), Sn(2), A64Condition::Eq));
    }
}

// Data Processing -- Scalar Floating-Point: the FCVT of Floating-point
// data-processing (1 source), and Conversion between floating-point and
// integer.
// sf | 0 | S | 11110 | ptype | 1 | rmode | opcode | 000000 | Rn | Rd
A64_VECTORS("AArch64 encodes conversions between floats and integers") {
    A64_GROUP("FCVT between the two precisions") {
        A64_ENCODES(0x1E22C020, Fcvt(Dn(0), Sn(1)));
        A64_ENCODES(0x1E624020, Fcvt(Sn(0), Dn(1)));
        A64_ENCODES(0x1E22C3FF, Fcvt(Dn(31), Sn(31)));

        // Converting to the precision already in hand is not an instruction.
        A64_REFUSES(InvalidRegister, Fcvt(Dn(0), Dn(1)));
        A64_REFUSES(InvalidRegister, Fcvt(Sn(0), Sn(1)));
        A64_REFUSES(InvalidRegister, Fcvt(Hn(0), Sn(1)));
        A64_REFUSES(InvalidRegister, Fcvt(Wn(0), Sn(1)));
    }

    A64_GROUP("float to integer, rounding toward zero") {
        A64_ENCODES(0x1E380020, Fcvtzs(Wn(0), Sn(1)));
        A64_ENCODES(0x1E780020, Fcvtzs(Wn(0), Dn(1)));
        A64_ENCODES(0x9E380020, Fcvtzs(Xn(0), Sn(1)));
        A64_ENCODES(0x9E780020, Fcvtzs(Xn(0), Dn(1)));
        A64_ENCODES(0x1E390020, Fcvtzu(Wn(0), Sn(1)));
        A64_ENCODES(0x1E790020, Fcvtzu(Wn(0), Dn(1)));
        A64_ENCODES(0x9E390020, Fcvtzu(Xn(0), Sn(1)));
        A64_ENCODES(0x9E790020, Fcvtzu(Xn(0), Dn(1)));
        A64_ENCODES(0x9E7803FE, Fcvtzs(Xn(30), Dn(31)));
        A64_ENCODES(0x1E39009F, Fcvtzu(Wzr, Sn(4)));

        // The direction is fixed by the instruction, so the operands cannot be
        // written the other way round and mean the opposite conversion.
        A64_REFUSES(InvalidRegister, Fcvtzs(Sn(0), Wn(1)));
        A64_REFUSES(InvalidRegister, Fcvtzs(Sp, Dn(1)));
        A64_REFUSES(InvalidRegister, Fcvtzu(Wn(0), Hn(1)));
    }

    A64_GROUP("integer to float") {
        A64_ENCODES(0x1E220020, Scvtf(Sn(0), Wn(1)));
        A64_ENCODES(0x9E220020, Scvtf(Sn(0), Xn(1)));
        A64_ENCODES(0x1E620020, Scvtf(Dn(0), Wn(1)));
        A64_ENCODES(0x9E620020, Scvtf(Dn(0), Xn(1)));
        A64_ENCODES(0x1E230020, Ucvtf(Sn(0), Wn(1)));
        A64_ENCODES(0x9E230020, Ucvtf(Sn(0), Xn(1)));
        A64_ENCODES(0x1E630020, Ucvtf(Dn(0), Wn(1)));
        A64_ENCODES(0x9E630020, Ucvtf(Dn(0), Xn(1)));
        A64_ENCODES(0x9E6203DF, Scvtf(Dn(31), Xn(30)));
        A64_ENCODES(0x1E2303E2, Ucvtf(Sn(2), Wzr));

        A64_REFUSES(InvalidRegister, Scvtf(Wn(0), Sn(1)));
        A64_REFUSES(InvalidRegister, Ucvtf(Dn(0), Sp));
        A64_REFUSES(InvalidRegister, Scvtf(Dn(0), Dn(1)));
    }
}

// Data Processing -- Scalar Floating-Point: Floating-point data-processing
// (1 source), the round-to-integral group.
// M | 0 | S | 11110 | ptype | 1 | opcode | 10000 | Rn | Rd
A64_VECTORS("AArch64 encodes floating-point rounding") {
    A64_ENCODES(0x1E244020, Frintn(Sn(0), Sn(1)));
    A64_ENCODES(0x1E644020, Frintn(Dn(0), Dn(1)));
    A64_ENCODES(0x1E24C020, Frintp(Sn(0), Sn(1)));
    A64_ENCODES(0x1E64C020, Frintp(Dn(0), Dn(1)));
    A64_ENCODES(0x1E254020, Frintm(Sn(0), Sn(1)));
    A64_ENCODES(0x1E654020, Frintm(Dn(0), Dn(1)));
    A64_ENCODES(0x1E25C020, Frintz(Sn(0), Sn(1)));
    A64_ENCODES(0x1E65C020, Frintz(Dn(0), Dn(1)));
    A64_ENCODES(0x1E264020, Frinta(Sn(0), Sn(1)));
    A64_ENCODES(0x1E664020, Frinta(Dn(0), Dn(1)));
    A64_ENCODES(0x1E65C3DF, Frintz(Dn(31), Dn(30)));
    A64_ENCODES(0x1E264230, Frinta(Sn(16), Sn(17)));

    A64_REFUSES(InvalidRegister, Frintn(Sn(0), Dn(1)));
    A64_REFUSES(InvalidRegister, Frintp(Xn(0), Xn(1)));
}

// The composite sequences, whose words are those of the instructions they are
// built from and whose interest is in which of them they choose.
A64_VECTORS("AArch64 loads a constant in the shortest sequence that reaches it") {
    A64_GROUP("one move where one reaches") {
        A64_EMITS((0xD2800000), LoadImm64(Xn(0), 0));
        A64_EMITS((0xD2800540), LoadImm64(Xn(0), 42));
        A64_EMITS((0xD29FFFE0), LoadImm64(Xn(0), 0xFFFF));
        A64_EMITS((0xD2A24680), LoadImm64(Xn(0), 0x12340000));
        // A value that is mostly ones is one MOVN rather than a chain, at any
        // halfword.
        A64_EMITS((0x92800001), LoadImm64(Xn(1), ~0ULL));
        A64_EMITS((0x92824681), LoadImm64(Xn(1), ~0x1234ULL));
        A64_EMITS((0x92C24666), LoadImm64(Xn(6), 0xFFFFEDCCFFFFFFFFULL));
        A64_EMITS((0xD2FFFFE0), LoadImm64(Xn(0), 0xFFFF000000000000ULL));
    }

    A64_GROUP("a MOVK for every halfword the root does not leave behind") {
        A64_EMITS((0xD28ACF02, 0xF2A24682), LoadImm64(Xn(2), 0x12345678));
        A64_EMITS((0xD299BDE2, 0xF2B21562, 0xF2CACF02, 0xF2E24682), LoadImm64(Xn(2), 0x1234567890ABCDEFULL));
    }

    A64_GROUP("one logical immediate where the chain would be longer") {
        A64_EMITS((0xB200F3E3), LoadImm64(Xn(3), 0x5555555555555555ULL));
        A64_EMITS((0xB2103FE3), LoadImm64(Xn(3), 0xFFFF0000FFFF0000ULL));
        // A run of ones that stays inside the low word encodes at either
        // width, and the register's own width is the canonical spelling.
        A64_EMITS((0xB27C5FE4), LoadImm64(Xn(4), 0x0FFFFFF0));
        // A value that fits in a word can also be loaded through the W view of
        // the register, which zeroes the rest of it: the halfwords above the
        // word cost nothing there, and the 32-bit logical immediates are a
        // different set from the 64-bit ones, since a rotation that would wrap
        // out of the word is one of theirs and none of the others.
        A64_EMITS((0x12AA8644), LoadImm64(Xn(4), 0xABCDFFFF));
        A64_EMITS((0x320C4FE4), LoadImm64(Xn(4), 0xFFF000FF));
    }

    A64_GROUP("a W destination takes the 32-bit chain") {
        A64_EMITS((0x5297DDE5, 0x72BBD5A5), LoadImm64(Wn(5), 0xDEADBEEF));
        A64_EMITS((0x52BFFFE5), LoadImm64(Wn(5), 0xFFFF0000));
        // Every bit set is no logical immediate at either width, since a run
        // of ones covering the register is also every rotation of itself, so
        // the value the pool would otherwise hold is one MOVN.
        A64_EMITS((0x12800005), LoadImm64(Wn(5), 0xFFFFFFFF));
    }

    A64_GROUP("refusals") {
        // A W register holds 32 bits and zeroes the rest of the X register it
        // is a view of, so a wider value is refused rather than truncated.
        A64_REFUSES(InvalidImmediate, LoadImm64(Wn(0), 0x100000000ULL));
        A64_REFUSES(InvalidRegister, LoadImm64(Sp, 0));
        A64_REFUSES(InvalidRegister, LoadImm64(Dn(0), 0));
    }
}

A64_VECTORS("AArch64 reaches a symbol through a page and an offset within it") {
    A64SymbolRef ref{};

    A64_GROUP("the address of a symbol is ADRP plus ADD") {
        A64_EMITS((0x90000009, 0x91000129), LoadAddress(Xn(9), ref));
        CHECK(ref.adrp == 0);
        CHECK(ref.lo12 == 4);
    }

    A64_GROUP("the relocation offsets name the instructions wherever they land") {
        CHECK(enc.Nop() == A64Status::Ok);
        CHECK(enc.LoadAddress(Xn(9), ref) == A64Status::Ok);
        CHECK(ref.adrp == 4);
        CHECK(ref.lo12 == 8);
        CHECK(enc.WordAt(ref.adrp) == 0x90000009);
        CHECK(enc.WordAt(ref.lo12) == 0x91000129);
    }

    A64_GROUP("the value at a symbol is ADRP plus the access itself") {
        // A load may name the register holding the page, since the address has
        // been read by the time the value is written.
        A64_EMITS((0x90000002, 0xF9400042), LoadFromSymbol(Xn(2), ref, Xn(2)));
        CHECK(ref.lo12 == 4);
        // The default base is the first intra-procedure-call scratch register.
        A64_EMITS((0x90000010, 0xFD400205), LoadFromSymbol(Dn(5), ref));
        A64_EMITS((0x90000010, 0xB9000201), StoreToSymbol(Wn(1), ref));
    }

    A64_GROUP("refusals") {
        // A store out of the register holding the page would have overwritten
        // the address before it was used.
        A64_REFUSES(InvalidRegister, StoreToSymbol(Xn(16), ref));
        // Neither reading of code 31 holds an address a sequence can use.
        A64_REFUSES(InvalidRegister, LoadAddress(Sp, ref));
        A64_REFUSES(InvalidRegister, LoadAddress(Xzr, ref));
        A64_REFUSES(InvalidRegister, LoadAddress(Wn(0), ref));
        A64_REFUSES(InvalidRegister, LoadFromSymbol(Xn(0), ref, Wn(3)));
        // The access is checked before the page address is emitted, so a
        // refusal leaves no half a sequence behind.
        A64_REFUSES(InvalidRegister, LoadFromSymbol(Sp, ref));
        A64_REFUSES(InvalidRegister, StoreToSymbol(Sp, ref));
    }
}

A64_VECTORS("AArch64 adds a value no immediate field can hold") {
    A64_GROUP("one instruction where the magnitude encodes") {
        A64_EMITS((0x91001020), AddSubLargeImm(Xn(0), Xn(1), 4));
        A64_EMITS((0xD1001020), AddSubLargeImm(Xn(0), Xn(1), -4));
        A64_EMITS((0xD14007FF), AddSubLargeImm(Sp, Sp, -4096));
        // Nothing to add is nothing to emit, unless the value has to move.
        A64_EMITS((), AddSubLargeImm(Xn(0), Xn(0), 0));
        A64_EMITS((0xAA0103E0), AddSubLargeImm(Xn(0), Xn(1), 0));
    }

    A64_GROUP("two where the two shift positions cover it between them") {
        A64_EMITS((0x91400420, 0x91000400), AddSubLargeImm(Xn(0), Xn(1), 0x1001));
        A64_EMITS((0xD1400420, 0xD1000400), AddSubLargeImm(Xn(0), Xn(1), -0x1001));
    }

    A64_GROUP("a scratch register where neither immediate form reaches") {
        A64_EMITS((0xD2A02010, 0x8B100020), AddSubLargeImm(Xn(0), Xn(1), 0x1000000));
        // Only the extended-register form can name SP, so an operand that is
        // the stack pointer takes it, reading the whole of the scratch.
        A64_EMITS((0xD2A02010, 0x8B3063FF), AddSubLargeImm(Sp, Sp, 0x1000000));
        // The most negative value has a magnitude, unlike in two's complement.
        A64_EMITS((0xD2F00010, 0xCB100020), AddSubLargeImm(Xn(0), Xn(1), INT64_MIN));
        // A caller holding something in the default scratch names another.
        A64_EMITS((0xD2A02011, 0x8B110200), AddSubLargeImm(Xn(0), Ip0, 0x1000000, Ip1));
    }

    A64_GROUP("refusals") {
        // The scratch is checked only on the path that reaches for it.
        A64_EMITS((0x91001200), AddSubLargeImm(Xn(0), Ip0, 4));
        A64_REFUSES(InvalidRegister, AddSubLargeImm(Xn(0), Ip0, 0x1000000));
        A64_REFUSES(InvalidRegister, AddSubLargeImm(Xn(0), Xn(1), 0x1000000, Sp));
        A64_REFUSES(InvalidRegister, AddSubLargeImm(Xn(0), Xn(1), 0x1000000, Xzr));
        A64_REFUSES(InvalidRegister, AddSubLargeImm(Wn(0), Wn(1), 4));
        A64_REFUSES(InvalidRegister, AddSubLargeImm(Xzr, Xn(1), 4));
    }
}

A64_VECTORS("AArch64 moves the stack pointer by a whole frame") {
    A64_EMITS((0xD10043FF), FrameAdjust(-16));
    A64_EMITS((0x910043FF), FrameAdjust(16));
    A64_EMITS((), FrameAdjust(0));
    // Each instruction of a multi-instruction adjustment moves SP by a multiple
    // of 16 in turn, so it is aligned between them as well as after them.
    A64_EMITS((0xD14407FF), FrameAdjust(-1052672));
    A64_EMITS((0xD2800210, 0xF2A02010, 0xCB3063FF), FrameAdjust(-16777232));

    A64_REFUSES(Unaligned, FrameAdjust(-24));
    A64_REFUSES(Unaligned, FrameAdjust(8));
}

A64_VECTORS("AArch64 probes each page of a large downward stack adjustment") {
    // Immediately below a page there is no guard page to advance, so this is
    // byte-for-byte the ordinary frame adjustment.
    A64_EMITS((0xD13FC3FF), ProbeStack(4080));

    // At the boundary the destination page is touched, and immediately above
    // it the aligned tail is applied only after that touch.
    A64_EMITS((0xD14007FF, 0xF90003FF), ProbeStack(4096));
    A64_EMITS((0xD14007FF, 0xF90003FF, 0xD10043FF), ProbeStack(4112));

    // Every complete page is entered and touched in order. No scratch register
    // is materialized regardless of the total size.
    A64_EMITS((0xD14007FF, 0xF90003FF, 0xD14007FF, 0xF90003FF, 0xD14007FF, 0xF90003FF), ProbeStack(12288));

    A64_EMITS((), ProbeStack(0));
    A64_REFUSES(Unaligned, ProbeStack(4088));
    A64_REFUSES(InvalidImmediate, ProbeStack(-16));
}

A64_VECTORS("AArch64 resolves a memory operand to an addressing mode that reaches it") {
    A64MemOperand mem;

    A64_GROUP("the scaled form where the offset divides and fits") {
        CHECK(enc.ResolveMemOperand(Sp, 32760, 8, mem) == A64Status::Ok);
        CHECK(mem.base == Sp);
        CHECK(mem.offset == 32760);
        CHECK_FALSE(mem.unscaled);
        CHECK(code.empty());
        A64_ENCODES(0xF97FFFE0, Ldr(Xn(0), mem.base, static_cast<std::uint64_t>(mem.offset)));
    }

    A64_GROUP("the unscaled form for what does not divide or is below the base") {
        CHECK(enc.ResolveMemOperand(Xn(1), 4, 8, mem) == A64Status::Ok);
        CHECK(mem.unscaled);
        CHECK(mem.offset == 4);
        CHECK(enc.ResolveMemOperand(Xn(1), -256, 8, mem) == A64Status::Ok);
        CHECK(mem.base == Xn(1));
        CHECK(mem.offset == -256);
        CHECK(mem.unscaled);
        CHECK(code.empty());
        A64_ENCODES(0xF8500020, Ldur(Xn(0), mem.base, mem.offset));
    }

    A64_GROUP("a scratch register for what neither form reaches") {
        CHECK(enc.ResolveMemOperand(Sp, 32768, 8, mem) == A64Status::Ok);
        CHECK(mem.base == Ip0);
        CHECK(mem.offset == 0);
        CHECK_FALSE(mem.unscaled);
        Emitted({0x914023F0});

        CHECK(enc.ResolveMemOperand(Sp, -257, 8, mem) == A64Status::Ok);
        CHECK(mem.base == Ip0);
        Emitted({0xD10407F0});

        // The scratch is the destination of its own adjustment, so the sequence
        // that materializes the address may take more than one instruction.
        CHECK(enc.ResolveMemOperand(Sp, 100000, 8, mem) == A64Status::Ok);
        Emitted({0x914063F0, 0x911A8210});
    }

    A64_GROUP("the width decides how far the scaled form reaches") {
        CHECK(enc.ResolveMemOperand(Xn(1), 4095, 1, mem) == A64Status::Ok);
        CHECK_FALSE(mem.unscaled);
        CHECK(mem.offset == 4095);
        CHECK(code.empty());
        // One byte further and the field is full, though a doubleword access
        // would still have thousands of its own to go.
        CHECK(enc.ResolveMemOperand(Xn(1), 4096, 1, mem) == A64Status::Ok);
        CHECK(mem.base == Ip0);
        Emitted({0x91400430});
    }

    A64_GROUP("refusals") {
        A64_REFUSES(InvalidRegister, ResolveMemOperand(Xzr, 0, 8, mem));
        A64_REFUSES(InvalidImmediate, ResolveMemOperand(Xn(1), 0, 3, mem));
        A64_REFUSES(InvalidImmediate, ResolveMemOperand(Xn(1), 0, 32, mem));
        // The scratch is refused where it would overwrite the base it is
        // computed from, and where it is no scratch at all.
        A64_REFUSES(InvalidRegister, ResolveMemOperand(Ip0, 32768, 8, mem));
        A64_REFUSES(InvalidRegister, ResolveMemOperand(Xn(1), 32768, 8, mem, Sp));
    }
}
