/**
 * @file timeline.h
 * @brief Estimates how a sequence of instructions flows through an in-order pipeline.
 *
 * Each instruction is given an issue slot. An instruction cannot issue before the one ahead of it,
 * and not before every register it reads has been produced far enough ahead (see cost_model.h).
 * When it would otherwise issue too early, bubbles are inserted before it. On a variant with
 * hazard detection those bubbles are the hardware's stall cycles; on a variant without it they are
 * the NOPs the programmer would have to add to get a correct result.
 */
#pragma once

#include <cstddef>
#include <vector>

#include "optimizer/cost_model.h"
#include "optimizer/program_model.h"

namespace Kites
{
namespace optimizer
{
struct Timeline
{
    /// Issue slot of the instruction at each position of the order (bubbles included).
    std::vector<int> issueSlot;

    /// Bubbles inserted immediately before the instruction at each position.
    std::vector<int> bubblesBefore;

    int bubbleCount{0};

    /// Total slots used: the number of instructions plus the bubbles.
    int slotCount{0};

    /// Consumer/register pairs that read their value too early in the sequence as written,
    /// without any bubbles. On a variant without hazard detection the program would compute a
    /// wrong result; on one with it, each is a stall.
    int violationCount{0};
};

/**
 * @brief Estimates the timeline of `order`, a sequence of instruction indices into
 * `model.instructions`.
 *
 * Only dependencies inside the sequence are considered; whatever ran before it is ignored.
 */
[[nodiscard]] Timeline estimateTimeline(const ProgramModel &model, const CostModel &costModel,
                                        const std::vector<size_t> &order);

} // namespace optimizer
} // namespace Kites
