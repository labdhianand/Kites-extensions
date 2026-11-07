/**
 * @file program_model.h
 * @brief Turns an assembled program into the structure the reordering optimizer works on:
 * decoded instructions, "units" (the instructions one source line expanded to), and basic blocks.
 *
 * A unit is the group of consecutive instructions that came from a single source line. Most lines
 * are one instruction, but pseudo-instructions expand to several (li, la, call, ...). The
 * optimizer treats a unit as indivisible, because the pieces of an expansion depend on each other
 * in ways the register analysis cannot see (and `la` bakes in its own address).
 */
#pragma once

#include <cstddef>
#include <vector>

#include "common/assembled_program.h"
#include "optimizer/instr_info.h"

namespace Kites
{
namespace optimizer
{
/**
 * @brief The instructions generated from one source line. Never split by the optimizer.
 */
struct Unit
{
    size_t firstInstruction{0};
    size_t instructionCount{0};
    unsigned sourceLine{0}; ///< 1-based source line, or 0 if the assembler gave none.

    /// A unit that reads its own address (auipc, as in `la`) must stay at its original position.
    bool readsPc{false};

    /// A unit containing ecall/ebreak/CSR access or an unrecognised word. Nothing may cross it.
    bool isBarrier{false};

    /// True if the unit's last instruction ends a basic block (branch, jump, ecall, ...).
    bool endsBlock{false};
};

/**
 * @brief A maximal run of units that is only ever entered at its first instruction and only left
 * at its last.
 */
struct BasicBlock
{
    size_t firstUnit{0};
    size_t unitCount{0};
    size_t firstInstruction{0};
    size_t instructionCount{0};

    /// True if the block's last instruction is a branch, jump, ecall or ebreak.
    bool hasTerminator{false};

    /// False if the block has an entry point other than its first instruction (a jump into the
    /// middle of a pseudo-instruction expansion). Such a block must be left exactly as written.
    bool isReorderable{true};
};

struct ProgramModel
{
    std::vector<InstrInfo> instructions; ///< Indexed by instruction number (address / 4).
    std::vector<Unit> units;
    std::vector<BasicBlock> blocks;
};

/**
 * @brief Decodes an assembled program and splits it into units and basic blocks.
 *
 * A basic block starts at the first instruction, at every statically known branch/jump target,
 * at every label in the text section (so indirect jumps through a label are safe), and after
 * every instruction that ends a block.
 */
[[nodiscard]] ProgramModel buildProgramModel(const AssembledProgram &program);

} // namespace optimizer
} // namespace Kites
