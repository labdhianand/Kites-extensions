/**
 * @file timeline.cpp
 * @brief Implementation of the in-order pipeline timeline estimator.
 */
#include "optimizer/timeline.h"

#include <algorithm>

namespace Kites
{
namespace optimizer
{
namespace
{
size_t registerSlot(RegRef reg)
{
    return (reg.file == RegFile::Fpr ? 32 : 0) + reg.index;
}
} // namespace

TimelineBuilder::Placement TimelineBuilder::place(const InstrInfo &info)
{
    int earliestSlot = m_previousSlot + 1;
    for (size_t s = 0; s < info.sourceCount; ++s)
    {
        const auto &write = m_lastWrite[registerSlot(info.sources[s])];
        if (!write)
        {
            continue;
        }
        earliestSlot = std::max(earliestSlot, write->issueSlot + write->minGap);

        // As written, with no bubbles, the instruction sits at slot == position.
        if (m_position - write->position < write->minGap)
        {
            ++m_violationCount;
        }
    }

    Placement placement;
    placement.bubbles = earliestSlot - (m_previousSlot + 1);
    placement.issueSlot = earliestSlot;
    m_bubbleCount += placement.bubbles;
    m_previousSlot = earliestSlot;

    if (info.dest.isValid())
    {
        m_lastWrite[registerSlot(info.dest)] =
            LastWrite{m_position, earliestSlot, m_costModel.minGap(info)};
    }
    ++m_position;
    return placement;
}

Timeline estimateTimeline(const ProgramModel &model, const CostModel &costModel,
                          const std::vector<size_t> &order)
{
    Timeline timeline;
    timeline.issueSlot.reserve(order.size());
    timeline.bubblesBefore.reserve(order.size());

    TimelineBuilder builder(costModel);
    for (size_t instruction : order)
    {
        const TimelineBuilder::Placement placement = builder.place(model.instructions[instruction]);
        timeline.issueSlot.push_back(placement.issueSlot);
        timeline.bubblesBefore.push_back(placement.bubbles);
    }

    timeline.bubbleCount = builder.bubbleCount();
    timeline.slotCount = builder.slotCount();
    timeline.violationCount = builder.violationCount();
    return timeline;
}

} // namespace optimizer
} // namespace Kites
