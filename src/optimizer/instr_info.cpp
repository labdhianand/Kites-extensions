/**
 * @file instr_info.cpp
 * @brief Implementation of the static instruction decoder used by the reordering optimizer.
 */
#include "optimizer/instr_info.h"

namespace Kites
{
namespace optimizer
{
namespace
{
constexpr uint8_t kRegA0 = 10;
constexpr uint8_t kRegA1 = 11;
constexpr uint8_t kRegA2 = 12;
constexpr uint8_t kRegA7 = 17;

int32_t signExtend(uint32_t value, int bitCount)
{
    const int shift = 32 - bitCount;
    return static_cast<int32_t>(value << shift) >> shift;
}

RegRef gpr(uint8_t index)
{
    return {RegFile::Gpr, index};
}

RegRef fpr(uint8_t index)
{
    return {RegFile::Fpr, index};
}

void addSource(InstrInfo &info, RegRef reg)
{
    // x0 is the constant zero: reading it never depends on an earlier instruction.
    if (reg.file == RegFile::Gpr && reg.index == 0)
    {
        return;
    }
    for (size_t i = 0; i < info.sourceCount; ++i)
    {
        if (info.sources[i] == reg)
        {
            return;
        }
    }
    info.sources[info.sourceCount++] = reg;
}

void setDest(InstrInfo &info, RegRef reg)
{
    // Writes to x0 are discarded, so they create no dependency either.
    if (reg.file == RegFile::Gpr && reg.index == 0)
    {
        info.dest = RegRef{};
        return;
    }
    info.dest = reg;
}

// Leaves the instruction marked as unrecognised. Callers return immediately afterwards.
void markUnknown(InstrInfo &info)
{
    info.klass = InstrClass::Unknown;
    info.isValid = false;
    info.sourceCount = 0;
    info.dest = RegRef{};
    info.isBarrier = true;
    info.endsBlock = true;
}

// Decodes the OP-FP major opcode (0b1010011). Returns false if the encoding is not recognised.
bool decodeFloatCompute(InstrInfo &info, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2,
                        uint8_t funct7)
{
    info.klass = InstrClass::FloatingPoint;

    // funct7 bit 0 selects single/double precision (fmt); the upper bits select the operation.
    switch (funct7 >> 1)
    {
    case 0b000000: // fadd
    case 0b000010: // fsub
    case 0b000100: // fmul
    case 0b000110: // fdiv
        addSource(info, fpr(rs1));
        addSource(info, fpr(rs2));
        setDest(info, fpr(rd));
        return true;
    case 0b010110: // fsqrt
        addSource(info, fpr(rs1));
        setDest(info, fpr(rd));
        return true;
    case 0b001000: // fsgnj, fsgnjn, fsgnjx
        if (funct3 > 2)
        {
            return false;
        }
        addSource(info, fpr(rs1));
        addSource(info, fpr(rs2));
        setDest(info, fpr(rd));
        return true;
    case 0b001010: // fmin, fmax
        if (funct3 > 1)
        {
            return false;
        }
        addSource(info, fpr(rs1));
        addSource(info, fpr(rs2));
        setDest(info, fpr(rd));
        return true;
    case 0b101000: // fle, flt, feq: compare two FPRs, result goes to a GPR
        if (funct3 > 2)
        {
            return false;
        }
        addSource(info, fpr(rs1));
        addSource(info, fpr(rs2));
        setDest(info, gpr(rd));
        return true;
    case 0b010000: // fcvt.s.d, fcvt.d.s
        addSource(info, fpr(rs1));
        setDest(info, fpr(rd));
        return true;
    case 0b110000: // fcvt.w/wu/l/lu: FPR to integer
        if (rs2 > 3)
        {
            return false;
        }
        addSource(info, fpr(rs1));
        setDest(info, gpr(rd));
        return true;
    case 0b110100: // fcvt.{s,d}.w/wu/l/lu: integer to FPR
        if (rs2 > 3)
        {
            return false;
        }
        addSource(info, gpr(rs1));
        setDest(info, fpr(rd));
        return true;
    case 0b111000: // fmv.x.w / fmv.x.d (funct3 0) and fclass (funct3 1): FPR to GPR
        if (rs2 != 0 || funct3 > 1)
        {
            return false;
        }
        addSource(info, fpr(rs1));
        setDest(info, gpr(rd));
        return true;
    case 0b111100: // fmv.w.x / fmv.d.x: GPR to FPR
        if (rs2 != 0 || funct3 != 0)
        {
            return false;
        }
        addSource(info, gpr(rs1));
        setDest(info, fpr(rd));
        return true;
    default:
        return false;
    }
}
} // namespace

InstrInfo decodeInstruction(uint32_t word, uint64_t pc)
{
    InstrInfo info;
    info.word = word;
    info.pc = pc;
    info.isValid = true;

    const uint8_t opcode = word & 0x7F;
    const uint8_t rd = (word >> 7) & 0x1F;
    const uint8_t funct3 = (word >> 12) & 0x07;
    const uint8_t rs1 = (word >> 15) & 0x1F;
    const uint8_t rs2 = (word >> 20) & 0x1F;
    const uint8_t rs3 = (word >> 27) & 0x1F;
    const uint8_t funct7 = (word >> 25) & 0x7F;

    const int32_t immI = signExtend(word >> 20, 12);
    const int32_t immS = signExtend(((word >> 25) << 5) | ((word >> 7) & 0x1F), 12);
    const int32_t immB = signExtend(((word >> 31) << 12) | (((word >> 7) & 0x1) << 11) |
                                        (((word >> 25) & 0x3F) << 5) | (((word >> 8) & 0xF) << 1),
                                    13);
    const int32_t immU = static_cast<int32_t>(word & 0xFFFFF000);
    const int32_t immJ = signExtend(((word >> 31) << 20) | (((word >> 12) & 0xFF) << 12) |
                                        (((word >> 20) & 0x1) << 11) |
                                        (((word >> 21) & 0x3FF) << 1),
                                    21);

    // The all-zero word is treated as a nop, like the disassembler does.
    if (word == 0)
    {
        info.klass = InstrClass::Alu;
        return info;
    }

    switch (opcode)
    {
    case 0b0110011: // OP: integer register-register (and M extension)
    case 0b0111011: // OP-32
        info.klass = (funct7 == 0b0000001) ? InstrClass::MulDiv : InstrClass::Alu;
        addSource(info, gpr(rs1));
        addSource(info, gpr(rs2));
        setDest(info, gpr(rd));
        break;

    case 0b0010011: // OP-IMM
    case 0b0011011: // OP-IMM-32
        info.klass = InstrClass::Alu;
        info.imm = immI;
        addSource(info, gpr(rs1));
        setDest(info, gpr(rd));
        break;

    case 0b0000011: // integer loads
    {
        static constexpr uint8_t kLoadSizes[8] = {1, 2, 4, 8, 1, 2, 4, 0};
        if (kLoadSizes[funct3] == 0)
        {
            markUnknown(info);
            return info;
        }
        info.klass = InstrClass::Load;
        info.imm = immI;
        info.memSize = kLoadSizes[funct3];
        info.memBase = gpr(rs1);
        addSource(info, gpr(rs1));
        setDest(info, gpr(rd));
        break;
    }

    case 0b0100011: // integer stores
    {
        static constexpr uint8_t kStoreSizes[8] = {1, 2, 4, 8, 0, 0, 0, 0};
        if (kStoreSizes[funct3] == 0)
        {
            markUnknown(info);
            return info;
        }
        info.klass = InstrClass::Store;
        info.imm = immS;
        info.memSize = kStoreSizes[funct3];
        info.memBase = gpr(rs1);
        addSource(info, gpr(rs1));
        addSource(info, gpr(rs2)); // the data being stored
        break;
    }

    case 0b0000111: // flw, fld
    {
        if (funct3 != 2 && funct3 != 3)
        {
            markUnknown(info);
            return info;
        }
        info.klass = InstrClass::Load;
        info.imm = immI;
        info.memSize = (funct3 == 2) ? 4 : 8;
        info.memBase = gpr(rs1);
        addSource(info, gpr(rs1));
        setDest(info, fpr(rd));
        break;
    }

    case 0b0100111: // fsw, fsd
    {
        if (funct3 != 2 && funct3 != 3)
        {
            markUnknown(info);
            return info;
        }
        info.klass = InstrClass::Store;
        info.imm = immS;
        info.memSize = (funct3 == 2) ? 4 : 8;
        info.memBase = gpr(rs1);
        addSource(info, gpr(rs1));
        addSource(info, fpr(rs2)); // the data being stored
        break;
    }

    case 0b1100011: // conditional branches
        if (funct3 == 0b010 || funct3 == 0b011)
        {
            markUnknown(info);
            return info;
        }
        info.klass = InstrClass::Branch;
        info.imm = immB;
        info.hasTarget = true;
        info.targetPc = pc + static_cast<int64_t>(immB);
        info.endsBlock = true;
        addSource(info, gpr(rs1));
        addSource(info, gpr(rs2));
        break;

    case 0b0110111: // lui
        info.klass = InstrClass::Alu;
        info.imm = immU;
        setDest(info, gpr(rd));
        break;

    case 0b0010111: // auipc
        info.klass = InstrClass::Alu;
        info.imm = immU;
        info.readsPc = true;
        setDest(info, gpr(rd));
        break;

    case 0b1101111: // jal
        info.klass = InstrClass::Jal;
        info.imm = immJ;
        info.hasTarget = true;
        info.targetPc = pc + static_cast<int64_t>(immJ);
        info.endsBlock = true;
        setDest(info, gpr(rd));
        break;

    case 0b1100111: // jalr
        info.klass = InstrClass::Jalr;
        info.imm = immI;
        info.endsBlock = true;
        addSource(info, gpr(rs1));
        setDest(info, gpr(rd));
        break;

    case 0b1110011: // SYSTEM: ecall, ebreak and the CSR instructions
        info.klass = InstrClass::System;
        info.isBarrier = true;
        if (funct3 == 0)
        {
            // ecall takes the syscall number in a7 and arguments in a0-a2, and a read syscall
            // returns its result in a0.
            info.endsBlock = true;
            addSource(info, gpr(kRegA7));
            addSource(info, gpr(kRegA0));
            addSource(info, gpr(kRegA1));
            addSource(info, gpr(kRegA2));
            setDest(info, gpr(kRegA0));
        }
        else
        {
            // csrrw/csrrs/csrrc read rs1; the immediate forms (funct3 >= 5) read no register.
            if (funct3 < 4)
            {
                addSource(info, gpr(rs1));
            }
            setDest(info, gpr(rd));
        }
        break;

    case 0b1000011: // fmadd
    case 0b1000111: // fmsub
    case 0b1001011: // fnmsub
    case 0b1001111: // fnmadd
        info.klass = InstrClass::FloatingPoint;
        addSource(info, fpr(rs1));
        addSource(info, fpr(rs2));
        addSource(info, fpr(rs3));
        setDest(info, fpr(rd));
        break;

    case 0b1010011: // OP-FP
        if (!decodeFloatCompute(info, rd, funct3, rs1, rs2, funct7))
        {
            markUnknown(info);
            return info;
        }
        break;

    default:
        markUnknown(info);
        return info;
    }

    return info;
}

} // namespace optimizer
} // namespace Kites
