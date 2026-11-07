/**
 * @file optimizer.h
 * @brief The reordering optimizer's entry point: assembly source in, suggested source out.
 *
 * The program is assembled, split into basic blocks, and each block is scheduled for the chosen
 * pipeline variant. The suggestion is returned as edited source text, so the programmer can read
 * it, diff it against the original and assemble it with the normal tools.
 */
#pragma once

#include <string>
#include <vector>

#include "optimizer/cost_model.h"
#include "optimizer/loop_unroller.h"

namespace Kites
{
namespace optimizer
{
struct OptimizeOptions
{
    PipelineVariant variant{PipelineVariant::H_F};

    /// Remove NOP padding and keep only the padding the variant really needs.
    bool dropNops{false};

    /// Unroll counted loops by up to this factor before scheduling (see loop_unroller.h). 1 means
    /// no unrolling. Unrolling changes the code size, so it is off unless asked for.
    int unrollFactor{1};
};

/// What happened to one basic block.
struct BlockSummary
{
    unsigned firstLine{0}; ///< 1-based source lines spanned by the block's instructions
    unsigned lastLine{0};
    bool changed{false};
    int originalSlots{0};  ///< estimated slots (instructions plus bubbles) before
    int optimizedSlots{0}; ///< and after
    std::string reason;    ///< why the block was left alone, when it was
};

struct OptimizeResult
{
    std::string source;  ///< the suggested source (identical to the input if nothing changed)
    bool changed{false};

    /// Loops found, and whether they were unrolled. Empty unless unrolling was asked for.
    std::vector<LoopReport> loops;

    /// The scheduled blocks. Their line numbers refer to the unrolled source when loops were
    /// unrolled.
    std::vector<BlockSummary> blocks;

    /// Sums over all blocks of the static slot estimates. These are per pass through each block,
    /// not weighted by how often a block runs.
    int originalSlots{0};
    int optimizedSlots{0};
};

/**
 * @brief Suggests a better instruction order for `source`.
 *
 * Only reorders inside basic blocks, so labels, branches and jumps keep their meaning. On variants
 * without hazard detection the padding NOPs the new order needs are inserted.
 *
 * @throws std::runtime_error if `source` does not assemble. The suggestion is assembled as well
 * as a safety net, and a failure there throws too, though it is not expected.
 */
[[nodiscard]] OptimizeResult optimizeSource(const std::string &source,
                                            const OptimizeOptions &options);

} // namespace optimizer
} // namespace Kites
