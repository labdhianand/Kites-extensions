/**
 * @file symbolic_value.cpp
 * @brief Implementation of the symbolic register values.
 */
#include "optimizer/symbolic_value.h"

namespace Kites
{
namespace optimizer
{
SymbolicValue SymbolicValue::ofConstant(uint64_t value)
{
    SymbolicValue result;
    result.constant = value;
    return result;
}

SymbolicValue SymbolicValue::ofSymbol(uint32_t symbol)
{
    SymbolicValue result;
    result.terms[symbol] = 1;
    return result;
}

SymbolicValue SymbolicValue::plus(const SymbolicValue &other) const
{
    SymbolicValue result = *this;
    result.constant += other.constant;
    for (const auto &[symbol, coefficient] : other.terms)
    {
        const uint64_t sum = result.terms[symbol] + coefficient;
        if (sum == 0)
        {
            result.terms.erase(symbol);
        }
        else
        {
            result.terms[symbol] = sum;
        }
    }
    return result;
}

SymbolicValue SymbolicValue::minus(const SymbolicValue &other) const
{
    return plus(other.times(~uint64_t{0})); // multiplying by -1 modulo 2^64
}

SymbolicValue SymbolicValue::times(uint64_t factor) const
{
    SymbolicValue result;
    result.constant = constant * factor;
    for (const auto &[symbol, coefficient] : terms)
    {
        const uint64_t product = coefficient * factor;
        if (product != 0)
        {
            result.terms[symbol] = product;
        }
    }
    return result;
}

SymbolicValue SymbolicValue::plusConstant(uint64_t value) const
{
    SymbolicValue result = *this;
    result.constant += value;
    return result;
}

SymbolicRegisters::SymbolicRegisters()
{
    for (uint32_t gpr = 1; gpr < 32; ++gpr)
    {
        m_values[gpr] = SymbolicValue::ofSymbol(gpr);
    }
    // m_values[0] stays the constant zero.
}

void SymbolicRegisters::execute(const InstrInfo &info, uint32_t opaqueSymbol)
{
    if (!info.dest.isValid() || info.dest.file != RegFile::Gpr)
    {
        return;
    }

    const uint32_t word = info.word;
    const uint8_t opcode = word & 0x7F;
    const uint8_t funct3 = (word >> 12) & 0x7;
    const uint8_t funct7 = (word >> 25) & 0x7F;
    const SymbolicValue &rs1 = m_values[(word >> 15) & 0x1F];
    const SymbolicValue &rs2 = m_values[(word >> 20) & 0x1F];
    const uint64_t imm = static_cast<uint64_t>(static_cast<int64_t>(info.imm));

    SymbolicValue result = SymbolicValue::ofSymbol(opaqueSymbol);
    if (opcode == 0b0010011 && funct3 == 0b000) // addi
    {
        result = rs1.plusConstant(imm);
    }
    else if (opcode == 0b0010011 && funct3 == 0b001 && (word >> 26) == 0) // slli
    {
        result = rs1.times(uint64_t{1} << ((word >> 20) & 0x3F));
    }
    else if (opcode == 0b0110011 && funct3 == 0b000 && funct7 == 0b0000000) // add
    {
        result = rs1.plus(rs2);
    }
    else if (opcode == 0b0110011 && funct3 == 0b000 && funct7 == 0b0100000) // sub
    {
        result = rs1.minus(rs2);
    }
    else if (opcode == 0b0110111) // lui (sign-extended to 64 bits)
    {
        result = SymbolicValue::ofConstant(imm);
    }

    m_values[info.dest.index] = result;
}

} // namespace optimizer
} // namespace Kites
