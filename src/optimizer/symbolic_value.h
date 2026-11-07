/**
 * @file symbolic_value.h
 * @brief Tracks what each integer register holds, symbolically, through a basic block.
 *
 * A value is a linear combination of symbols plus a constant, for example
 * `x10@entry + 4 * x12@entry + 8`. Symbols 0..31 stand for the registers' values when the block
 * starts. Any result the analysis cannot see through (a load, a multiply, a 32-bit `w` operation)
 * gets a fresh symbol of its own. The memory dependency analysis uses this to prove that two
 * accesses never touch the same bytes even when they go through different base registers: if two
 * addresses have identical symbolic parts, their difference is a known constant.
 *
 * Arithmetic wraps modulo 2^64, exactly like the registers.
 */
#pragma once

#include <array>
#include <cstdint>
#include <map>

#include "optimizer/instr_info.h"

namespace Kites
{
namespace optimizer
{
struct SymbolicValue
{
    std::map<uint32_t, uint64_t> terms; ///< symbol -> coefficient; zero coefficients never stored
    uint64_t constant{0};

    [[nodiscard]] static SymbolicValue ofConstant(uint64_t value);
    [[nodiscard]] static SymbolicValue ofSymbol(uint32_t symbol);

    [[nodiscard]] SymbolicValue plus(const SymbolicValue &other) const;
    [[nodiscard]] SymbolicValue minus(const SymbolicValue &other) const;
    [[nodiscard]] SymbolicValue times(uint64_t factor) const;
    [[nodiscard]] SymbolicValue plusConstant(uint64_t value) const;

    /// True if the two values differ by a known constant.
    [[nodiscard]] bool sameTermsAs(const SymbolicValue &other) const
    {
        return terms == other.terms;
    }

    bool operator==(const SymbolicValue &other) const = default;
};

/**
 * @brief The symbolic contents of the 32 integer registers, updated one instruction at a time in
 * program order.
 */
class SymbolicRegisters
{
  public:
    /// Every register starts as its own entry symbol; x0 is the constant zero.
    SymbolicRegisters();

    [[nodiscard]] const SymbolicValue &value(uint8_t gpr) const
    {
        return m_values[gpr];
    }

    /// Applies `info`'s write. `opaqueSymbol` names its result if it cannot be expressed
    /// linearly; it must be unique to this instruction and at least 32.
    void execute(const InstrInfo &info, uint32_t opaqueSymbol);

  private:
    std::array<SymbolicValue, 32> m_values;
};

} // namespace optimizer
} // namespace Kites
