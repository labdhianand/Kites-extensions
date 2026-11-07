/**
 * @file scheduler.h
 * @brief Reorders the units of one basic block to remove pipeline bubbles.
 *
 * The scheduler only ever picks orders that respect the block's dependency graph, so the result
 * computes exactly what the original did. Among the legal orders it uses greedy list scheduling:
 * at every step it places the ready unit that adds the fewest bubbles right now, preferring the one
 * on the longest remaining dependency chain when several tie. If the result is not strictly better
 * than the original order it returns the original, so a suggestion is never a regression.
 */
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "optimizer/cost_model.h"
#include "optimizer/liveness.h"
#include "optimizer/program_model.h"

namespace Kites
{
namespace optimizer
{
struct ScheduleOptions
{
    /// Remove instructions that do nothing (NOP padding) and keep only the padding the cost model
    /// really needs. This is what turns hand-written padding on a no-hazard-detection variant into
    /// useful work.
    bool dropNops{false};

    /// Registers that may still be read after this block ends (see liveness.h). On a variant
    /// without hazard detection only these need their last write kept far enough from the end of
    /// the block. If unset, every register is assumed live, which is always safe.
    std::optional<RegisterSet> liveOut;
};

struct BlockSchedule
{
    /// Block-local unit indices in the suggested order. Dropped NOP units are not listed.
    std::vector<size_t> unitOrder;

    /// NOP units that were removed (block-local indices, ascending).
    std::vector<size_t> droppedUnits;

    /// Bubbles the model needs before the first instruction of each unit in `unitOrder`. On a
    /// variant with hazard detection these are stalls; without it they are NOPs to insert. Bubbles
    /// that fall inside a multi-instruction unit are counted in the totals but not listed here,
    /// since a source line cannot be padded in the middle.
    std::vector<int> bubblesBefore;

    int originalSlots{0};
    int scheduledSlots{0};
    int originalBubbles{0};
    int scheduledBubbles{0};

    /// False if the original order was kept.
    bool changed{false};

    /// Why the block was left as it is, when it was. Empty if it was scheduled or there was simply
    /// nothing to improve.
    std::string reason;

    [[nodiscard]] int savedSlots() const
    {
        return originalSlots - scheduledSlots;
    }
};

/**
 * @brief Schedules one basic block for the given pipeline variant.
 *
 * Blocks that cannot be reordered safely (entered in the middle of a pseudo-instruction, or
 * containing floating-point instructions, which the pipelined models do not support) are returned
 * unchanged with a reason.
 */
[[nodiscard]] BlockSchedule scheduleBlock(const ProgramModel &model, const BasicBlock &block,
                                          const CostModel &costModel,
                                          const ScheduleOptions &options = {});

} // namespace optimizer
} // namespace Kites
