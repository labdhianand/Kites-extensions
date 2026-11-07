/**
 * @file optimizer.cpp
 * @brief Implementation of the source-to-source reordering optimizer.
 */
#include "optimizer/optimizer.h"

#include <map>
#include <sstream>
#include <stdexcept>

#include "assembler/assembler.h"
#include "optimizer/liveness.h"
#include "optimizer/program_model.h"
#include "optimizer/scheduler.h"
#include "optimizer/source_text.h"

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

        std::string prefix = slotLine.prefix;
        const int padding = insertPadding ? schedule.bubblesBefore[slot] : 0;
        for (int i = 0; i < padding; ++i)
        {
            output.push_back(prefix + kPaddingLine);
            prefix = leadingWhitespace(slotLine.prefix); // the label, if any, came first
        }
        output.push_back(prefix + bodySource.body);
        replacements[line] = output;
    }
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

OptimizeResult optimizeSource(const std::string &original, const OptimizeOptions &options)
{
    OptimizeResult result;

    // Unrolling happens first, so the scheduler sees the longer loop bodies.
    std::string source = original;
    if (options.unrollFactor > 1)
    {
        UnrollOptions unrollOptions;
        unrollOptions.maxFactor = options.unrollFactor;
        UnrollResult unrolled = unrollLoops(original, unrollOptions);
        source = std::move(unrolled.source);
        result.loops = std::move(unrolled.loops);
        result.changed = unrolled.changed;
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

    bool rescheduled = false;
    for (size_t b = 0; b < model.blocks.size(); ++b)
    {
        const BasicBlock &block = model.blocks[b];

        ScheduleOptions scheduleOptions;
        scheduleOptions.dropNops = options.dropNops;
        scheduleOptions.liveOut = liveness[b].liveOut;
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

    checkAssembles(result.source, "the suggested program does not assemble");
    return result;
}

} // namespace optimizer
} // namespace Kites
