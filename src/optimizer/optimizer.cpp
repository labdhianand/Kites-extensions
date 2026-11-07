/**
 * @file optimizer.cpp
 * @brief Implementation of the source-to-source reordering optimizer.
 */
#include "optimizer/optimizer.h"

#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "assembler/assembler.h"
#include "optimizer/liveness.h"
#include "optimizer/program_model.h"
#include "optimizer/scheduler.h"

namespace Kites
{
namespace optimizer
{
namespace
{
// The program text as lines, remembering how to put it back together exactly.
struct SourceText
{
    std::vector<std::string> lines;
    std::string newline{"\n"};
    bool endsWithNewline{false};
};

SourceText splitSource(const std::string &text)
{
    SourceText result;
    result.newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    result.endsWithNewline = !text.empty() && text.back() == '\n';

    size_t start = 0;
    while (start < text.size())
    {
        const size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? end : end - start);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        result.lines.push_back(std::move(line));
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return result;
}

std::string joinSource(const SourceText &text)
{
    std::string joined;
    for (size_t i = 0; i < text.lines.size(); ++i)
    {
        if (i > 0)
        {
            joined += text.newline;
        }
        joined += text.lines[i];
    }
    if (text.endsWithNewline && !text.lines.empty())
    {
        joined += text.newline;
    }
    return joined;
}

// A source line split into what must stay at its position (indentation and any labels) and the
// instruction with its trailing comment, which is what moves.
struct SplitLine
{
    std::string prefix;
    std::string body;
};

SplitLine splitLine(const std::string &line)
{
    static const std::regex prefixPattern(R"(^\s*(?:[A-Za-z_.$][A-Za-z0-9_.$]*\s*:\s*)*)");
    std::smatch match;
    std::regex_search(line, match, prefixPattern);
    const std::string prefix = match.str(0);
    return {prefix, line.substr(prefix.size())};
}

std::string leadingWhitespace(const std::string &prefix)
{
    return prefix.substr(0, prefix.find_first_not_of(" \t"));
}

bool hasLabel(const std::string &prefix)
{
    return prefix.find_first_not_of(" \t") != std::string::npos;
}

std::string rightTrimmed(std::string text)
{
    const size_t end = text.find_last_not_of(" \t");
    text.erase(end == std::string::npos ? 0 : end + 1);
    return text;
}

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

OptimizeResult optimizeSource(const std::string &source, const OptimizeOptions &options)
{
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
    const bool insertPadding = !costModel.detectsHazards();

    const SourceText text = splitSource(source);
    std::map<unsigned, std::vector<std::string>> replacements;

    OptimizeResult result;
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
            result.changed = true;
        }
    }

    if (!result.changed)
    {
        result.source = source;
        return result;
    }

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
