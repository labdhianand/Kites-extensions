/**
 * @file constant_propagation.cpp
 * @brief Implementation of the constant propagation over basic blocks.
 */
#include "optimizer/constant_propagation.h"

namespace Kites
{
namespace optimizer
{
namespace
{
ConstantRegisters unknownRegisters()
{
    ConstantRegisters registers{};
    registers[0] = 0;
    return registers;
}

// The result of `info` if all of its inputs are known and it is an operation we can compute.
std::optional<uint64_t> evaluate(const InstrInfo &info, const ConstantRegisters &registers)
{
    const uint32_t word = info.word;
    const uint8_t opcode = word & 0x7F;
    const uint8_t funct3 = (word >> 12) & 0x7;
    const uint8_t funct7 = (word >> 25) & 0x7F;
    const std::optional<uint64_t> rs1 = registers[(word >> 15) & 0x1F];
    const std::optional<uint64_t> rs2 = registers[(word >> 20) & 0x1F];
    const uint64_t imm = static_cast<uint64_t>(static_cast<int64_t>(info.imm));

    if (opcode == 0b0110111) // lui
    {
        return imm;
    }
    if (opcode == 0b0010011 && rs1) // OP-IMM
    {
        const unsigned shamt = (word >> 20) & 0x3F;
        switch (funct3)
        {
        case 0b000:
            return *rs1 + imm; // addi
        case 0b100:
            return *rs1 ^ imm; // xori
        case 0b110:
            return *rs1 | imm; // ori
        case 0b111:
            return *rs1 & imm; // andi
        case 0b001:
            return *rs1 << shamt; // slli
        case 0b101:
            return (word >> 30) & 1
                       ? static_cast<uint64_t>(static_cast<int64_t>(*rs1) >> shamt) // srai
                       : *rs1 >> shamt;                                              // srli
        default:
            return std::nullopt;
        }
    }
    if (opcode == 0b0110011 && rs1 && rs2 && (funct7 == 0 || funct7 == 0b0100000)) // OP
    {
        switch (funct3)
        {
        case 0b000:
            return funct7 == 0 ? *rs1 + *rs2 : *rs1 - *rs2; // add / sub
        case 0b100:
            return *rs1 ^ *rs2;
        case 0b110:
            return *rs1 | *rs2;
        case 0b111:
            return *rs1 & *rs2;
        default:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

ConstantRegisters meet(const ConstantRegisters &a, const ConstantRegisters &b)
{
    ConstantRegisters result{};
    for (size_t r = 0; r < 32; ++r)
    {
        if (a[r] && b[r] && *a[r] == *b[r])
        {
            result[r] = a[r];
        }
    }
    return result;
}
} // namespace

ConstantRegisters runBlockOnConstants(const ProgramModel &model, const BasicBlock &block,
                                      ConstantRegisters entry)
{
    for (size_t k = block.firstInstruction; k < block.firstInstruction + block.instructionCount;
         ++k)
    {
        const InstrInfo &info = model.instructions[k];
        if (info.dest.isValid() && info.dest.file == RegFile::Gpr)
        {
            entry[info.dest.index] = evaluate(info, entry);
        }
    }
    entry[0] = 0;
    return entry;
}

std::vector<BlockConstants> propagateConstants(const ProgramModel &model, const ControlFlow &flow)
{
    const size_t blockCount = model.blocks.size();
    std::vector<BlockConstants> constants(blockCount);

    bool changed = true;
    while (changed)
    {
        changed = false;
        for (size_t b = 0; b < blockCount; ++b)
        {
            // Meet over every way in. The program start and unknown jumps bring nothing known.
            std::optional<ConstantRegisters> entry;
            if (b == 0 || flow.hasUnknownPredecessors[b])
            {
                entry = unknownRegisters();
            }
            for (size_t predecessor : flow.predecessors[b])
            {
                if (!constants[predecessor].reachable)
                {
                    continue; // not reached yet; it will be met on a later pass if it is
                }
                entry = entry ? meet(*entry, constants[predecessor].atExit)
                              : constants[predecessor].atExit;
            }
            if (!entry)
            {
                continue;
            }

            const ConstantRegisters exit = runBlockOnConstants(model, model.blocks[b], *entry);
            if (!constants[b].reachable || constants[b].atEntry != *entry ||
                constants[b].atExit != exit)
            {
                constants[b].reachable = true;
                constants[b].atEntry = *entry;
                constants[b].atExit = exit;
                changed = true;
            }
        }
    }
    return constants;
}

} // namespace optimizer
} // namespace Kites
