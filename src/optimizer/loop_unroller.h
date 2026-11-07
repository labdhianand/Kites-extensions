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

} // namespace optimizer
} // namespace Kites
