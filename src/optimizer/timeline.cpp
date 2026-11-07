/**
 * @file timeline.cpp
 * @brief Implementation of the in-order pipeline timeline estimator.
 */
#include "optimizer/timeline.h"

#include <algorithm>
#include <array>
#include <optional>

namespace Kites
{
namespace optimizer
{
namespace
{
constexpr size_t kRegisterSlotCount = 64; // 32 GPRs followed by 32 FPRs

size_t registerSlot(RegRef reg)
{
    return (reg.file == RegFile::Fpr ? 32 : 0) + reg.index;
}

// The most recent instruction in the sequence that wrote a register.
struct LastWrite
{
    int position{0}; ///< index in the order, which is also its slot if no bubbles were added
    int issueSlot{0};
    int minGap{1};
};
} // namespace

Timeline estimateTimeline(const ProgramModel &model, const CostModel &costModel,
                          const std::vector<size_t> &order)
{
    Timeline timeline;
    timeline.issueSlot.reserve(order.size());
    timeline.bubblesBefore.reserve(order.size());

    std::array<std::optional<LastWrite>, kRegisterSlotCount> lastWrite{};
    int previousSlot = -1;

    for (size_t position = 0; position < order.size(); ++position)
    {
        const InstrInfo &info = model.instructions[order[position]];

        int earliestSlot = previousSlot + 1;
        for (size_t s = 0; s < info.sourceCount; ++s)
        {
            const auto &write = lastWrite[registerSlot(info.sources[s])];
            if (!write)
            {
                continue;
            }
            earliestSlot = std::max(earliestSlot, write->issueSlot + write->minGap);

            // As written, with no bubbles, the instruction sits at slot == position.
            if (static_cast<int>(position) - write->position < write->minGap)
            {
                ++timeline.violationCount;
            }
        }

        const int bubbles = earliestSlot - (previousSlot + 1);
        timeline.bubblesBefore.push_back(bubbles);
        timeline.bubbleCount += bubbles;
        timeline.issueSlot.push_back(earliestSlot);
        previousSlot = earliestSlot;

        if (info.dest.isValid())
        {
            lastWrite[registerSlot(info.dest)] = LastWrite{static_cast<int>(position),
                                                           earliestSlot, costModel.minGap(info)};
        }
    }

    timeline.slotCount = previousSlot + 1;
    return timeline;
}

} // namespace optimizer
} // namespace Kites
