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

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "optimizer/cost_model.h"
#include "optimizer/program_model.h"

namespace Kites
{
namespace optimizer
{
/**
 * @brief Places instructions one at a time. Cheap to copy, so a scheduler can try a candidate on a
 * copy and keep the copy only if it likes the result.
 */
class TimelineBuilder
{
  public:
    struct Placement
    {
        int issueSlot{0};
        int bubbles{0}; ///< empty slots inserted immediately before this instruction
    };

    explicit TimelineBuilder(const CostModel &costModel) : m_costModel(costModel)
    {
    }

    /// Appends `info` to the sequence and returns where it landed.
    Placement place(const InstrInfo &info);

    /// Total slots used so far: the instructions placed plus the bubbles.
    [[nodiscard]] int slotCount() const
    {
        return m_previousSlot + 1;
    }

    [[nodiscard]] int bubbleCount() const
    {
        return m_bubbleCount;
    }

    /// Consumer/register pairs that read their value too early in the sequence as written, with no
    /// bubbles inserted.
    [[nodiscard]] int violationCount() const
    {
        return m_violationCount;
    }

  private:
    static constexpr size_t kRegisterSlotCount = 64; // 32 GPRs followed by 32 FPRs

    // The most recent instruction that wrote a register.
    struct LastWrite
    {
        int position{0}; ///< index in the sequence, which is also its slot if no bubbles were added
        int issueSlot{0};
        int minGap{1};
    };

    CostModel m_costModel;
    std::array<std::optional<LastWrite>, kRegisterSlotCount> m_lastWrite{};
    int m_previousSlot{-1};
    int m_position{0};
    int m_bubbleCount{0};
    int m_violationCount{0};
};

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
