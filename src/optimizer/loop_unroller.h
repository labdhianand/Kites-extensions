/**
 * @file loop_unroller.h
 * @brief Unrolls simple counted loops so the scheduler has more independent work to interleave
 * and the pipeline pays fewer taken-branch penalties.
 *
 * A loop is unrolled only when doing so is provably exact:
 *  - it is a single basic block that branches back to its own start;
 *  - its branch compares a counter, updated once per iteration by `addi iv, iv, c`, with a
 *    register the loop never writes (or x0);
 *  - the counter's start value and the bound are known constants on every way into the loop, so
 *    the number of iterations is known;
 *  - the unroll factor divides that number, so the branches dropped from the middle of the
 *    unrolled body would all have been taken.
 * Every copy but the last renames the registers an iteration writes before reading them to
 * registers the program never uses, which removes false dependencies between the copies. The last
 * copy keeps the original names, so every value read after the loop is still correct. The only
 * visible difference afterwards is in those previously unused registers.
 */
#pragma once

#include <string>
#include <vector>

namespace Kites
{
namespace optimizer
{
struct UnrollOptions
{
    /// The largest unroll factor to try. The largest factor up to this that divides the trip
    /// count is used.
    int maxFactor{4};
    /// Remove the address arithmetic the copies repeat (see address_folding.h).
    bool foldAddresses{true};
    /// Let registers the program never reads after a loop end with different values. Without
    /// this every register keeps its final value, which leaves little to fold: the last copy
    /// still has to compute its own addresses into the original registers.
    bool allowDeadRegisterChanges{false};
};

/// What happened to one loop.
struct LoopReport
{
    unsigned firstLine{0}; ///< 1-based source lines of the loop's instructions, in the input
    unsigned lastLine{0};
    bool unrolled{false};
    int factor{1};
    long tripCount{-1}; ///< -1 if it could not be determined
    std::vector<unsigned> renamedTo; ///< previously unused registers the copies now use
    int foldedInstructions{0};       ///< repeated address arithmetic removed from the copies
    /// Registers (not read after the loop) that now end with a different value; only with
    /// allowDeadRegisterChanges.
    std::vector<unsigned> changedDeadRegisters;
    std::string reason;              ///< why the loop was left alone, when it was
};

struct UnrollResult
{
    std::string source;
    bool changed{false};
    std::vector<LoopReport> loops;
};

/**
 * @brief Unrolls every loop of `source` that can be unrolled exactly.
 * @throws std::runtime_error if `source` does not assemble.
 */
[[nodiscard]] UnrollResult unrollLoops(const std::string &source, const UnrollOptions &options);

struct PointerOptions
{
    /// Let registers nothing reads after the loop end with different values.
    bool allowDeadRegisterChanges{false};
    /// Registers earlier passes took into use (the unroller's renames): their final values are
    /// not part of the result, so they never need keeping.
    std::vector<unsigned> optimizerRegisters;
};

/// One pointer introduced into a loop.
struct PointerReport
{
    unsigned firstLine{0}; ///< the loop's lines, in the pass's input
    unsigned lastLine{0};
    unsigned pointer{0}; ///< the new register, which steps through memory
    long stride{0};      ///< bytes it moves per iteration
    int removed{0};      ///< instructions removed from the loop body (one step was added)
    /// Registers nothing reads after the loop that now end with a different value.
    std::vector<unsigned> changedDeadRegisters;
};

struct PointerResult
{
    std::string source;
    bool changed{false};
    std::vector<PointerReport> pointers;
};

/**
 * @brief Turns indexed addressing in loops into a pointer that steps through memory.
 *
 * In a single-block loop where a register `iv` changes only by `addi iv, iv, c`, an address
 * computed as `B + (iv << k)` (with `B` never written in the loop) is replaced by a new register
 * `p`: `p = B + (iv << k)` is computed once just before the loop, `addi p, p, c << k` steps it in
 * the loop, and the loads and stores use `p` with adjusted offsets. The `slli`/`add` pair leaves
 * the loop when nothing else needs its result (see address_folding.h for the checks). A pointer is
 * kept only if the loop gets shorter. The loop must be entered only by falling into it from the
 * block before, which is where `p` is computed.
 *
 * @throws std::runtime_error if `source` does not assemble.
 */
[[nodiscard]] PointerResult introducePointers(const std::string &source,
                                              const PointerOptions &options);

/// True for the registers a function must preserve for its caller (s0-s11: x8, x9, x18-x27).
/// The unroller uses them only when no other register is free.
[[nodiscard]] bool isCalleeSaved(unsigned reg);

} // namespace optimizer
} // namespace Kites
