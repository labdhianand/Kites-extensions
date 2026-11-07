/**
 * @file scheduler.cpp
 * @brief Implementation of the list scheduler.
 */
#include "optimizer/scheduler.h"

#include <algorithm>
#include <array>

#include "optimizer/dependency_graph.h"
#include "optimizer/timeline.h"

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

// A unit that does nothing at all: no registers read or written, no memory access, no control flow.
// This is `addi x0, x0, 0` and its relatives, i.e. padding.
bool isNopUnit(const ProgramModel &model, const Unit &unit)
{
    for (size_t k = unit.firstInstruction; k < unit.firstInstruction + unit.instructionCount; ++k)
    {
        const InstrInfo &info = model.instructions[k];
        if (info.sourceCount != 0 || info.dest.isValid() || info.isMemory() || info.isBarrier ||
            info.endsBlock || info.readsPc)
        {
            return false;
        }
    }
    return true;
}

bool usesFloatingPoint(const ProgramModel &model, const Unit &unit)
{
    for (size_t k = unit.firstInstruction; k < unit.firstInstruction + unit.instructionCount; ++k)
    {
        const InstrInfo &info = model.instructions[k];
        if (info.klass == InstrClass::FloatingPoint || info.dest.file == RegFile::Fpr)
        {
            return true;
        }
        for (size_t s = 0; s < info.sourceCount; ++s)
        {
            if (info.sources[s].file == RegFile::Fpr)
            {
                return true;
            }
        }
    }
    return false;
}

// The instruction indices of the units in `unitOrder`, back to back.
std::vector<size_t> flatten(const ProgramModel &model, const BasicBlock &block,
                            const std::vector<size_t> &unitOrder)
{
    std::vector<size_t> instructions;
    for (size_t node : unitOrder)
    {
        const Unit &unit = model.units[block.firstUnit + node];
        for (size_t k = unit.firstInstruction; k < unit.firstInstruction + unit.instructionCount;
             ++k)
        {
            instructions.push_back(k);
        }
    }
    return instructions;
}

// Bubbles before the first instruction of each unit, given the timeline of the flattened order.
std::vector<int> bubblesBeforeUnits(const ProgramModel &model, const BasicBlock &block,
                                    const std::vector<size_t> &unitOrder,
                                    const Timeline &timeline)
{
    std::vector<int> bubbles;
    size_t position = 0;
    for (size_t node : unitOrder)
    {
        bubbles.push_back(timeline.bubblesBefore[position]);
        position += model.units[block.firstUnit + node].instructionCount;
    }
    return bubbles;
}

// How long the chain of dependent units starting at each unit is, so that units whose delay would
// hold up the most later work are placed first. Edges go forward, so walking the units backwards
// visits every successor before its predecessor.
std::vector<int> chainLengths(const ProgramModel &model, const BasicBlock &block,
                              const DependencyGraph &graph, const CostModel &costModel)
{
    std::vector<int> length(graph.nodeCount, 0);
    for (size_t node = graph.nodeCount; node-- > 0;)
    {
        const Unit &unit = model.units[block.firstUnit + node];
        const InstrInfo &lastInstruction = model.instructions[unit.firstInstruction +
                                                              unit.instructionCount - 1];
        int longest = static_cast<int>(unit.instructionCount);
        for (size_t edgeIndex : graph.outgoing[node])
        {
            const Dependency &edge = graph.edges[edgeIndex];
            const int weight = edge.kind == DependencyKind::RegisterRaw
                                   ? costModel.minGap(lastInstruction)
                                   : 1;
            longest = std::max(longest, weight + length[edge.to]);
        }
        length[node] = longest;
    }
    return length;
}

// What a block looks like from the outside: how close each register's last write is to the end
// of the block, and how soon each register the block reads from before it is first read. The
// neighbouring blocks' producers and consumers are unknown, so these distances are what protects
// them.
struct BoundaryProfile
{
    std::array<int, kRegisterSlotCount> exitDistance{}; ///< -1 if the block never writes it
    std::array<int, kRegisterSlotCount> firstReadSlot{}; ///< -1 if the block never reads it live-in

    BoundaryProfile()
    {
        exitDistance.fill(-1);
        firstReadSlot.fill(-1);
    }
};

BoundaryProfile boundaryProfileOf(const ProgramModel &model,
                                  const std::vector<size_t> &instructions,
                                  const Timeline &timeline)
{
    BoundaryProfile profile;
    std::array<int, kRegisterSlotCount> lastWriteSlot{};
    lastWriteSlot.fill(-1);

    for (size_t position = 0; position < instructions.size(); ++position)
    {
        const InstrInfo &info = model.instructions[instructions[position]];
        const int slot = timeline.issueSlot[position];
        for (size_t s = 0; s < info.sourceCount; ++s)
        {
            const size_t reg = registerSlot(info.sources[s]);
            if (lastWriteSlot[reg] < 0 && profile.firstReadSlot[reg] < 0)
            {
                profile.firstReadSlot[reg] = slot;
            }
        }
        if (info.dest.isValid())
        {
            lastWriteSlot[registerSlot(info.dest)] = slot;
        }
    }

    for (size_t reg = 0; reg < kRegisterSlotCount; ++reg)
    {
        if (lastWriteSlot[reg] >= 0)
        {
            profile.exitDistance[reg] = timeline.slotCount - 1 - lastWriteSlot[reg];
        }
    }
    return profile;
}

// A producer in a neighbouring block and a consumer here are `exit + 1 + slot` slots apart, and
// anything at least `maxGap` apart is safe. So it is enough that each distance either stays at
// least as large as before or is already `maxGap - 1`, the point from which the other side cannot
// matter. Only needed where the hardware does not stall for the consumer. A register that nothing
// after the block reads needs no protection on the way out.
bool preservesBoundary(const BoundaryProfile &before, const BoundaryProfile &after, int maxGap,
                       const RegisterSet &liveOut)
{
    const int safe = maxGap - 1;
    for (size_t reg = 0; reg < kRegisterSlotCount; ++reg)
    {
        if (liveOut[reg] && before.exitDistance[reg] >= 0 &&
            after.exitDistance[reg] < std::min(before.exitDistance[reg], safe))
        {
            return false;
        }
        if (before.firstReadSlot[reg] >= 0 &&
            after.firstReadSlot[reg] < std::min(before.firstReadSlot[reg], safe))
        {
            return false;
        }
    }
    return true;
}

struct Attempt
{
    std::vector<size_t> order;
    std::vector<size_t> droppedUnits;
    Timeline timeline;
};

// Greedy list scheduling of the block. Dropped units (NOP padding) are left out entirely.
Attempt listSchedule(const ProgramModel &model, const BasicBlock &block,
                     const DependencyGraph &graph, const std::vector<int> &chain,
                     const CostModel &costModel, bool dropNops)
{
    Attempt attempt;
    std::vector<bool> dropped(block.unitCount, false);
    if (dropNops)
    {
        for (size_t node = 0; node < block.unitCount; ++node)
        {
            if (isNopUnit(model, model.units[block.firstUnit + node]))
            {
                dropped[node] = true;
                attempt.droppedUnits.push_back(node);
            }
        }
    }

    // A unit is ready once every unit it must follow has been placed. Dropped units are gone, so
    // they hold nothing up.
    std::vector<size_t> remainingPredecessors(block.unitCount, 0);
    for (const Dependency &edge : graph.edges)
    {
        if (!dropped[edge.from])
        {
            ++remainingPredecessors[edge.to];
        }
    }
    std::vector<size_t> ready;
    for (size_t node = 0; node < block.unitCount; ++node)
    {
        if (!dropped[node] && remainingPredecessors[node] == 0)
        {
            ready.push_back(node);
        }
    }

    TimelineBuilder state(costModel);
    while (!ready.empty())
    {
        size_t bestIndex = 0;
        int bestBubbles = 0;
        for (size_t candidateIndex = 0; candidateIndex < ready.size(); ++candidateIndex)
        {
            const size_t node = ready[candidateIndex];
            const Unit &unit = model.units[block.firstUnit + node];

            TimelineBuilder trial = state;
            int bubbles = 0;
            for (size_t k = unit.firstInstruction;
                 k < unit.firstInstruction + unit.instructionCount; ++k)
            {
                bubbles += trial.place(model.instructions[k]).bubbles;
            }

            // Fewest bubbles first; then the longest chain behind it; then original order.
            bool better = candidateIndex == 0 || bubbles < bestBubbles;
            if (!better && bubbles == bestBubbles)
            {
                const size_t best = ready[bestIndex];
                better = chain[node] > chain[best] || (chain[node] == chain[best] && node < best);
            }
            if (better)
            {
                bestIndex = candidateIndex;
                bestBubbles = bubbles;
            }
        }

        const size_t node = ready[bestIndex];
        ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(bestIndex));
        attempt.order.push_back(node);

        const Unit &unit = model.units[block.firstUnit + node];
        for (size_t k = unit.firstInstruction; k < unit.firstInstruction + unit.instructionCount;
             ++k)
        {
            state.place(model.instructions[k]);
        }
        for (size_t edgeIndex : graph.outgoing[node])
        {
            const size_t successor = graph.edges[edgeIndex].to;
            if (--remainingPredecessors[successor] == 0)
            {
                ready.push_back(successor);
            }
        }
    }

    attempt.timeline = estimateTimeline(model, costModel, flatten(model, block, attempt.order));
    return attempt;
}

BlockSchedule keepOriginal(const std::vector<size_t> &identity, const Timeline &original,
                           const ProgramModel &model, const BasicBlock &block, std::string reason)
{
    BlockSchedule schedule;
    schedule.unitOrder = identity;
    schedule.bubblesBefore = bubblesBeforeUnits(model, block, identity, original);
    schedule.originalSlots = schedule.scheduledSlots = original.slotCount;
    schedule.originalBubbles = schedule.scheduledBubbles = original.bubbleCount;
    schedule.reason = std::move(reason);
    return schedule;
}
} // namespace

BlockSchedule scheduleBlock(const ProgramModel &model, const BasicBlock &block,
                            const CostModel &costModel, const ScheduleOptions &options)
{
    std::vector<size_t> identity(block.unitCount);
    for (size_t i = 0; i < identity.size(); ++i)
    {
        identity[i] = i;
    }
    const std::vector<size_t> originalInstructions = flatten(model, block, identity);
    const Timeline original = estimateTimeline(model, costModel, originalInstructions);

    if (!block.isReorderable)
    {
        return keepOriginal(identity, original, model, block,
                            "the block can be entered in the middle of a pseudo-instruction");
    }
    for (size_t node = 0; node < block.unitCount; ++node)
    {
        if (usesFloatingPoint(model, model.units[block.firstUnit + node]))
        {
            return keepOriginal(identity, original, model, block,
                                "floating-point instructions are not supported on the pipelined "
                                "processor models");
        }
    }
    if (block.unitCount < 2)
    {
        return keepOriginal(identity, original, model, block, "");
    }

    const DependencyGraph graph = buildDependencyGraph(model, block);
    const std::vector<int> chain = chainLengths(model, block, graph, costModel);
    const BoundaryProfile originalBoundary =
        boundaryProfileOf(model, originalInstructions, original);

    // Try what was asked for first. If that would disturb the neighbouring blocks, fall back to
    // keeping the NOPs, which are then just ordinary fillers.
    std::vector<bool> attempts = {options.dropNops};
    if (options.dropNops)
    {
        attempts.push_back(false);
    }

    bool refusedForBoundary = false;
    for (bool dropNops : attempts)
    {
        const Attempt attempt = listSchedule(model, block, graph, chain, costModel, dropNops);
        if (attempt.timeline.slotCount >= original.slotCount)
        {
            continue;
        }

        // A hazard-detecting variant stalls for a neighbour's producer or consumer anyway; the
        // others silently compute a wrong value, so the block must look the same from outside.
        if (!costModel.detectsHazards())
        {
            const BoundaryProfile boundary = boundaryProfileOf(
                model, flatten(model, block, attempt.order), attempt.timeline);
            RegisterSet liveOut;
            liveOut.set(); // unknown successors: assume everything is read
            if (options.liveOut)
            {
                liveOut = *options.liveOut;
            }
            if (!preservesBoundary(originalBoundary, boundary, costModel.maxGap(), liveOut))
            {
                refusedForBoundary = true;
                continue;
            }
        }

        BlockSchedule schedule;
        schedule.unitOrder = attempt.order;
        schedule.droppedUnits = attempt.droppedUnits;
        schedule.bubblesBefore = bubblesBeforeUnits(model, block, attempt.order, attempt.timeline);
        schedule.originalSlots = original.slotCount;
        schedule.scheduledSlots = attempt.timeline.slotCount;
        schedule.originalBubbles = original.bubbleCount;
        schedule.scheduledBubbles = attempt.timeline.bubbleCount;
        schedule.changed = true;
        return schedule;
    }

    return keepOriginal(identity, original, model, block,
                        refusedForBoundary ? "a better order would change how close this block's "
                                             "values are to the neighbouring blocks, which the "
                                             "hardware does not protect"
                                           : "");
}

} // namespace optimizer
} // namespace Kites
