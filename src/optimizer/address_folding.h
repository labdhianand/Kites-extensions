/**
 * @file address_folding.h
 * @brief Removes address arithmetic that an unrolled loop body repeats.
 *
 * After a loop is unrolled, every copy of the body recomputes its addresses from the counter
 * (`slli t, i, 2; add p, base, t`) and steps the counter (`addi i, i, 1`). The copies' addresses
 * differ from the first copy's only by a constant, so a later copy can reuse the first copy's
 * address register with a larger offset (`lw x, 4(p)`), and the counter can be stepped once.
 *
 * The pass works on a straight-line list of instructions. It tracks every integer register as a
 * symbolic value, an expression plus a constant offset: `addi`, `slli`, `add` and `sub` stay in that
 * form, everything else (loads, multiplies, ...) is an opaque value tied to the instruction that
 * made it. An address computation is removed only if afterwards
 *  - every remaining instruction computes exactly the same symbolic value as before (a later `addi`
 *    or load/store may be pointed at another register holding the same expression, with its
 *    immediate adjusted by the difference),
 *  - every memory access uses the same address and stores the same value, every branch compares the
 *    same values, and every other instruction reads the same values,
 *  - and every register in `keepAtEnd` ends with the same value.
 * The order of memory accesses never changes, because only arithmetic is removed.
 */
#pragma once

#include <bitset>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "optimizer/instr_info.h"

namespace Kites
{
namespace optimizer
{
enum class FoldKind : uint8_t
{
    AddImmediate,     ///< addi rd, rs1, imm
    ShiftLeftImmediate, ///< slli rd, rs1, imm
    Add,              ///< add rd, rs1, rs2
    Subtract,         ///< sub rd, rs1, rs2
    Load,             ///< rd <- imm(rs1)
    Store,            ///< rs2 -> imm(rs1)
    Branch,           ///< compares rs1 with rs2
    Other             ///< reads `sources`, writes `rd` (if not 0) with an opaque value
};

/// One instruction of the straight-line code, with the registers it uses in this copy.
struct FoldInstr
{
    FoldKind kind{FoldKind::Other};
    unsigned rd{0};
    unsigned rs1{0};
    unsigned rs2{0};
    int64_t imm{0};
    unsigned funct3{0};            ///< loads and stores: the access width
    std::vector<unsigned> sources; ///< Other: every integer register read
    /// False if the instruction shares its source line with others (an expanded pseudo-
    /// instruction): it is then never removed or rewritten.
    bool rewritable{true};

    bool removed{false};
    bool rewritten{false}; ///< rs1 and imm changed (only addi, loads and stores)
};

/// Describes `info` with its integer registers renamed by `renames`.
[[nodiscard]] FoldInstr makeFoldInstr(const InstrInfo &info,
                                      const std::map<unsigned, unsigned> &renames, bool rewritable);

/// The assembly for a rewritten instruction (canonical register names, no comment).
[[nodiscard]] std::string foldedAssembly(const FoldInstr &instr);

struct FoldResult
{
    int removed{0};
    /// Registers that now end with a different value (never one in keepAtEnd).
    std::bitset<32> changedAtEnd;
};

/**
 * @brief Removes the arithmetic that can be removed, rewriting the instructions that read it.
 * @param code The straight-line code; instructions are marked removed or rewritten in place.
 * @param keepAtEnd The registers whose final value must not change.
 */
FoldResult foldAddresses(std::vector<FoldInstr> &code, const std::bitset<32> &keepAtEnd);

} // namespace optimizer
} // namespace Kites
