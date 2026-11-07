/**
 * @file instr_info.h
 * @brief Static decoding of RV64 I/M/F/D instructions into the facts the code-reordering
 * optimizer needs: which registers an instruction reads and writes, what memory access it makes,
 * and whether it affects control flow.
 *
 * This is pure logic with no dependency on Qt or on the processor models, so it can be used from
 * the CLI, the GUI and the tests alike.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Kites
{
namespace optimizer
{
/**
 * @brief Which architectural register file a register belongs to.
 *
 * The integer and floating-point files are separate: x5 and f5 are different registers, so a
 * write to one never creates a dependency for a read of the other.
 */
enum class RegFile : uint8_t
{
    None,
    Gpr,
    Fpr
};

struct RegRef
{
    RegFile file{RegFile::None};
    uint8_t index{0};

    [[nodiscard]] bool isValid() const
    {
        return file != RegFile::None;
    }

    bool operator==(const RegRef &other) const = default;
};

/**
 * @brief Coarse instruction category, used by the cost model and the dependency analysis.
 *
 * Floating-point loads and stores are classified as Load / Store (their data register is an FPR);
 * every other floating-point instruction is FloatingPoint.
 */
enum class InstrClass : uint8_t
{
    Alu,
    MulDiv,
    Load,
    Store,
    Branch,
    Jal,
    Jalr,
    System,
    FloatingPoint,
    Unknown
};

/**
 * @brief Everything the optimizer needs to know about one decoded instruction.
 */
struct InstrInfo
{
    /// Largest number of register sources of any instruction (ecall implicitly reads four).
    static constexpr size_t kMaxSourceCount = 4;

    uint32_t word{0}; ///< The raw 32-bit instruction.
    uint64_t pc{0};   ///< Address of the instruction.

    InstrClass klass{InstrClass::Unknown};
    bool isValid{false}; ///< False if the word is not a recognised instruction.

    /// Registers read. GPR x0 is never listed (it is the constant zero, so no dependency), and
    /// duplicates are removed.
    std::array<RegRef, kMaxSourceCount> sources{};
    size_t sourceCount{0};

    /// Register written. Invalid (RegFile::None) if there is none, or if the target is GPR x0.
    RegRef dest{};

    int32_t imm{0}; ///< Decoded immediate (the byte offset for loads/stores/branches/jumps).

    /// For Load/Store only: the access size in bytes and the base address register. The base is
    /// kept here even when it is x0, because x0 + offset is a usable absolute address.
    uint8_t memSize{0};
    RegRef memBase{};

    /// For Branch and Jal only: the statically known target address.
    bool hasTarget{false};
    uint64_t targetPc{0};

    /// True for auipc: the result depends on the instruction's own address, so it must not be
    /// moved.
    bool readsPc{false};

    /// Nothing may be moved across a barrier (ecall, ebreak, CSR access, unknown instructions).
    bool isBarrier{false};

    /// True if this instruction ends a basic block (branches, jumps, ecall, ebreak, unknown).
    bool endsBlock{false};

    [[nodiscard]] bool isMemory() const
    {
        return klass == InstrClass::Load || klass == InstrClass::Store;
    }
};

/**
 * @brief Decodes one 32-bit instruction word.
 *
 * Never throws. A word that is not a recognised instruction is returned with isValid == false and
 * is treated conservatively (a barrier that ends its block).
 *
 * @param word The raw instruction.
 * @param pc The address the instruction is located at (used to resolve branch/jump targets).
 */
[[nodiscard]] InstrInfo decodeInstruction(uint32_t word, uint64_t pc);

} // namespace optimizer
} // namespace Kites
