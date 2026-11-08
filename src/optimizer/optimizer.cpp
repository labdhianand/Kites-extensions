/**
 * @file optimizer.cpp
 * @brief Implementation of the source-to-source reordering optimizer.
 */
#include "optimizer/optimizer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <sstream>
#include <stdexcept>

#include "assembler/assembler.h"
#include "optimizer/liveness.h"
#include "optimizer/program_model.h"
#include "optimizer/scheduler.h"
#include "optimizer/source_text.h"
#include "optimizer/timeline.h"

namespace Kites
{
namespace optimizer
{
namespace
{
constexpr const char *kPaddingLine = "addi x0, x0, 0  # padding";

// What replaces each instruction line of a changed block. Lines map to zero, one or several
// lines (a dropped NOP, an ordinary line, a line preceded by padding).
void rewriteBlock(const SourceText &source, const ProgramModel &model, const BasicBlock &block,
                  const BlockSchedule &schedule, bool insertPadding,
                  std::map<unsigned, std::vector<std::string>> &replacements)
{
    // The source line of each unit of the block, in original order. These are the slots the
    // reordered instructions are poured into.
    std::vector<unsigned> slotLines;
    for (size_t node = 0; node < block.unitCount; ++node)
    {
        slotLines.push_back(model.units[block.firstUnit + node].sourceLine);
    }

    for (size_t slot = 0; slot < slotLines.size(); ++slot)
    {
        const unsigned line = slotLines[slot];
        const SplitLine slotLine = splitLine(source.lines.at(line - 1));
        std::vector<std::string> output;

        if (slot >= schedule.unitOrder.size())
        {
            // A dropped NOP. Keep any label that was on it, since jumps may land there.
            if (hasLabel(slotLine.prefix))
            {
                output.push_back(rightTrimmed(slotLine.prefix));
            }
            replacements[line] = output;
            continue;
        }

        const unsigned bodyLine = model.units[block.firstUnit + schedule.unitOrder[slot]].sourceLine;
        const SplitLine bodySource = splitLine(source.lines.at(bodyLine - 1));

        // The end-of-block padding goes before the branch, or after the last instruction.
        const bool lastSlot = slot + 1 == schedule.unitOrder.size();
        const int tail = insertPadding && lastSlot ? schedule.tailPadding : 0;

        std::string prefix = slotLine.prefix;
        const int padding =
            (insertPadding ? schedule.bubblesBefore[slot] : 0) + (block.hasTerminator ? tail : 0);
        for (int i = 0; i < padding; ++i)
        {
            output.push_back(prefix + kPaddingLine);
            prefix = leadingWhitespace(slotLine.prefix); // the label, if any, came first
        }
        output.push_back(prefix + bodySource.body);
        for (int i = 0; !block.hasTerminator && i < tail; ++i)
        {
            output.push_back(leadingWhitespace(slotLine.prefix) + kPaddingLine);
        }
        replacements[line] = output;
    }
}

size_t registerSlot(RegRef reg)
{
    return (reg.file == RegFile::Fpr ? 32 : 0) + reg.index;
}

// The block in its original order with just the NOPs a variant without hazard detection needs:
// what the timeline asks for inside the block, and enough at the end that every register a later
// block may read was written at least maxGap - 1 slots before the block ends. A consumer at the
// start of the next block is then always far enough away, whatever block it is.
void padBlock(const SourceText &source, const ProgramModel &model, const BasicBlock &block,
              const CostModel &costModel, const RegisterSet &liveOut,
              std::map<unsigned, std::vector<std::string>> &replacements)
{
    std::vector<size_t> instructions;
    for (size_t k = block.firstInstruction; k < block.firstInstruction + block.instructionCount; ++k)
    {
        instructions.push_back(k);
    }
    const Timeline timeline = estimateTimeline(model, costModel, instructions);

    // Padding can only go before a whole source line.
    for (size_t node = 0; node < block.unitCount; ++node)
    {
        const Unit &unit = model.units[block.firstUnit + node];
        for (size_t k = unit.firstInstruction + 1; k < unit.firstInstruction + unit.instructionCount;
             ++k)
        {
            if (timeline.bubblesBefore[k - block.firstInstruction] > 0)
            {
                throw std::runtime_error("line " + std::to_string(unit.sourceLine) +
                                         " would need padding in the middle of a "
                                         "pseudo-instruction, which a source line cannot express");
            }
        }
    }

    // Registers the terminator writes itself (the link register of a call) cannot be helped by
    // padding placed before it; nothing reads them straight away in practice.
    const size_t terminator = block.hasTerminator ? block.instructionCount - 1 : SIZE_MAX;
    std::array<int, 64> lastWriteSlot{};
    lastWriteSlot.fill(-1);
    for (size_t position = 0; position < instructions.size(); ++position)
    {
        const InstrInfo &info = model.instructions[instructions[position]];
        if (info.dest.isValid() && position != terminator)
        {
            lastWriteSlot[registerSlot(info.dest)] = timeline.issueSlot[position];
        }
    }
    const int safeDistance = costModel.maxGap() - 1;
    int tail = 0;
    for (size_t reg = 0; reg < 64; ++reg)
    {
        if (liveOut[reg] && lastWriteSlot[reg] >= 0)
        {
            const int exitDistance = timeline.slotCount - 1 - lastWriteSlot[reg];
            tail = std::max(tail, safeDistance - exitDistance);
        }
    }

    for (size_t node = 0; node < block.unitCount; ++node)
    {
        const Unit &unit = model.units[block.firstUnit + node];
        const SplitLine line = splitLine(source.lines.at(unit.sourceLine - 1));
        const bool isTerminator = block.hasTerminator && node + 1 == block.unitCount;
        const int padding = timeline.bubblesBefore[unit.firstInstruction - block.firstInstruction] +
                            (isTerminator ? tail : 0);

        std::vector<std::string> output;
        std::string prefix = line.prefix;
        for (int i = 0; i < padding; ++i)
        {
            output.push_back(prefix + kPaddingLine);
            prefix = leadingWhitespace(line.prefix); // the label, if any, came first
        }
        output.push_back(prefix + line.body);
        if (!block.hasTerminator && node + 1 == block.unitCount)
        {
            for (int i = 0; i < tail; ++i)
            {
                output.push_back(leadingWhitespace(line.prefix) + kPaddingLine);
            }
        }
        replacements[unit.sourceLine] = output;
    }
}

// For a variant without hazard detection: how soon each block may first read each register.
//
// A value produced `exit` slots before the end of a block that runs just before this one, and read
// at `slot` here, is `exit + 1 + slot` slots away and needs at least maxGap. The scheduler never
// lets a live-out register's exit distance drop below min(original exit, maxGap - 1), so that
// lower bound is what each predecessor guarantees. A predecessor that does not write the register
// at all puts its whole length between the producer and this block. A block entered from somewhere
// unknown gets the worst case; the program start has no earlier producer at all.
std::vector<std::array<int, 64>> earliestFirstReads(const ProgramModel &model,
                                                    const CostModel &costModel)
{
    const int safe = costModel.maxGap() - 1;
    const size_t blockCount = model.blocks.size();
    const ControlFlow flow = computeControlFlow(model);
    const std::vector<BlockLiveness> liveness = computeLiveness(model);

    // What each block guarantees on the way out, register by register.
    std::vector<std::array<int, 64>> guaranteedExit(blockCount);
    for (size_t b = 0; b < blockCount; ++b)
    {
        const BasicBlock &block = model.blocks[b];
        std::vector<size_t> instructions;
        for (size_t k = block.firstInstruction; k < block.firstInstruction + block.instructionCount;
             ++k)
        {
            instructions.push_back(k);
        }
        const Timeline timeline = estimateTimeline(model, costModel, instructions);
        guaranteedExit[b].fill(std::min(timeline.slotCount, safe));
        for (size_t position = 0; position < instructions.size(); ++position)
        {
            const InstrInfo &info = model.instructions[instructions[position]];
            if (info.dest.isValid())
            {
                guaranteedExit[b][registerSlot(info.dest)] =
                    std::min(timeline.slotCount - 1 - timeline.issueSlot[position], safe);
            }
        }
    }

    std::vector<std::array<int, 64>> earliest(blockCount);
    for (size_t b = 0; b < blockCount; ++b)
    {
        earliest[b].fill(0);
        for (size_t reg = 0; reg < 64; ++reg)
        {
            if (!liveness[b].liveIn[reg])
            {
                continue;
            }
            int closestExit = safe; // the program start: nothing was produced just before
            if (flow.hasUnknownPredecessors[b])
            {
                closestExit = 0;
            }
            for (size_t predecessor : flow.predecessors[b])
            {
                closestExit = std::min(closestExit, guaranteedExit[predecessor][reg]);
            }
            earliest[b][reg] = safe - closestExit;
        }
    }
    return earliest;
}

void checkAssembles(const std::string &source, const char *what)
{
    std::istringstream stream(source);
    try
    {
        (void)assemble(stream);
    }
    catch (const std::exception &error)
    {
        throw std::runtime_error(std::string(what) + ": " + error.what());
    }
}
} // namespace

std::vector<unsigned> registersNotCompared(const OptimizeResult &result)
{
    std::vector<unsigned> registers;
    for (const LoopReport &loop : result.loops)
    {
        registers.insert(registers.end(), loop.renamedTo.begin(), loop.renamedTo.end());
        registers.insert(registers.end(), loop.changedDeadRegisters.begin(),
                         loop.changedDeadRegisters.end());
    }
    for (const PointerReport &pointer : result.pointers)
    {
        registers.push_back(pointer.pointer);
        registers.insert(registers.end(), pointer.changedDeadRegisters.begin(),
                         pointer.changedDeadRegisters.end());
    }
    return registers;
}

OptimizeResult optimizeSource(const std::string &original, const OptimizeOptions &options)
{
    OptimizeResult result;

    // Unrolling happens first, so the scheduler sees the longer loop bodies.
    std::string source = original;
    if (options.unrollFactor > 1)
    {
        UnrollOptions unrollOptions;
        unrollOptions.maxFactor = options.unrollFactor;
        unrollOptions.allowDeadRegisterChanges = options.allowDeadRegisterChanges;
        UnrollResult unrolled = unrollLoops(original, unrollOptions);
        source = std::move(unrolled.source);
        result.loops = std::move(unrolled.loops);
        result.changed = unrolled.changed;
    }
    if (options.unrollFactor > 1 || options.allowDeadRegisterChanges)
    {
        PointerOptions pointerOptions;
        pointerOptions.allowDeadRegisterChanges = options.allowDeadRegisterChanges;
        for (const LoopReport &loop : result.loops)
        {
            pointerOptions.optimizerRegisters.insert(pointerOptions.optimizerRegisters.end(),
                                                     loop.renamedTo.begin(), loop.renamedTo.end());
        }
        PointerResult pointers = introducePointers(source, pointerOptions);
        source = std::move(pointers.source);
        result.pointers = std::move(pointers.pointers);
        result.changed = result.changed || pointers.changed;
    }

    // On a variant without hazard detection the scheduler trusts its input to be correctly padded:
    // a block it cannot improve is written out unchanged. Unrolling, folding and pointers can
    // remove the instructions that used to stand between a producer and its consumer, so pad the
    // transformed program again first. If that is impossible (padding would be needed inside a
    // pseudo-instruction), keep the loops as they were.
    if (source != original && !options.padOnly && !CostModel{options.variant}.detectsHazards())
    {
        OptimizeOptions pad;
        pad.variant = options.variant;
        pad.loadExtraLatency = options.loadExtraLatency;
        pad.padOnly = true;
        try
        {
            source = optimizeSource(source, pad).source;
        }
        catch (const std::exception &)
        {
            source = original;
            result.loops.clear();
            result.pointers.clear();
            result.changed = false;
        }
    }

    std::istringstream stream(source);
    const AssembledProgram program = [&]
    {
        try
        {
            return assemble(stream);
        }
        catch (const std::exception &error)
        {
            throw std::runtime_error(std::string("the program does not assemble: ") + error.what());
        }
    }();

    const ProgramModel model = buildProgramModel(program);
    const std::vector<BlockLiveness> liveness = computeLiveness(model);

    CostModel costModel;
    costModel.variant = options.variant;
    costModel.loadExtraLatency = options.loadExtraLatency;
    const bool insertPadding = !costModel.detectsHazards();

    const SourceText text = splitSource(source);
    std::map<unsigned, std::vector<std::string>> replacements;

    const std::vector<std::array<int, 64>> firstReads =
        insertPadding ? earliestFirstReads(model, costModel) : std::vector<std::array<int, 64>>{};

    bool rescheduled = false;
    if (options.padOnly)
    {
        // Keep the order; only pad, and only where the hardware does not stall by itself.
        if (insertPadding)
        {
            for (size_t b = 0; b < model.blocks.size(); ++b)
            {
                padBlock(text, model, model.blocks[b], costModel, liveness[b].liveOut,
                         replacements);
            }
            rescheduled = true;
        }
    }
    for (size_t b = 0; !options.padOnly && b < model.blocks.size(); ++b)
    {
        const BasicBlock &block = model.blocks[b];

        ScheduleOptions scheduleOptions;
        scheduleOptions.dropNops = options.dropNops;
        scheduleOptions.liveOut = liveness[b].liveOut;
        if (insertPadding)
        {
            scheduleOptions.earliestFirstRead = firstReads[b];
        }
        const BlockSchedule schedule = scheduleBlock(model, block, costModel, scheduleOptions);

        BlockSummary summary;
        summary.firstLine = model.units[block.firstUnit].sourceLine;
        summary.lastLine = model.units[block.firstUnit + block.unitCount - 1].sourceLine;
        summary.changed = schedule.changed;
        summary.originalSlots = schedule.originalSlots;
        summary.optimizedSlots = schedule.scheduledSlots;
        summary.reason = schedule.reason;
        result.blocks.push_back(summary);
        result.originalSlots += schedule.originalSlots;
        result.optimizedSlots += schedule.scheduledSlots;

        if (schedule.changed)
        {
            rewriteBlock(text, model, block, schedule, insertPadding, replacements);
            rescheduled = true;
        }
    }

    if (!rescheduled)
    {
        result.source = source;
        return result;
    }
    result.changed = true;

    SourceText rewritten;
    rewritten.newline = text.newline;
    rewritten.endsWithNewline = text.endsWithNewline;
    for (size_t i = 0; i < text.lines.size(); ++i)
    {
        const auto replacement = replacements.find(static_cast<unsigned>(i + 1));
        if (replacement == replacements.end())
        {
            rewritten.lines.push_back(text.lines[i]);
        }
        else
        {
            rewritten.lines.insert(rewritten.lines.end(), replacement->second.begin(),
                                   replacement->second.end());
        }
    }
    result.source = joinSource(rewritten);
    result.changed = result.source != original;

    checkAssembles(result.source, "the suggested program does not assemble");
    return result;
}

} // namespace optimizer
} // namespace Kites
