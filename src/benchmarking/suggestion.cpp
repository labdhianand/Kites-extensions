/**
 * @file suggestion.cpp
 * @brief Implementation of a complete reordering suggestion.
 */
#include "benchmarking/suggestion.h"

#include <stdexcept>

#include "optimizer/source_text.h"

namespace Kites
{
namespace benchmarking
{
namespace
{
optimizer::PipelineVariant variantOf(ProcessorType processor)
{
    switch (processor)
    {
    case ProcessorType::RV5Stage_NH_NF:
        return optimizer::PipelineVariant::NH_NF;
    case ProcessorType::RV5Stage_H_NF:
        return optimizer::PipelineVariant::H_NF;
    case ProcessorType::RV5Stage_NH_F:
        return optimizer::PipelineVariant::NH_F;
    default:
        return optimizer::PipelineVariant::H_F;
    }
}

int countPaddingLines(const std::string &text)
{
    int count = 0;
    for (const std::string &line : optimizer::splitSource(text).lines)
    {
        count += line.find("# padding") != std::string::npos ? 1 : 0;
    }
    return count;
}
} // namespace

bool canSuggestFor(ProcessorType processor)
{
    return processor == ProcessorType::RV5Stage_NH_NF || processor == ProcessorType::RV5Stage_H_NF ||
           processor == ProcessorType::RV5Stage_NH_F || processor == ProcessorType::RV5Stage_H_F;
}

std::string describe(ProcessorType processor)
{
    switch (processor)
    {
    case ProcessorType::RV5Stage_NH_NF:
        return "no hazard detection, no forwarding";
    case ProcessorType::RV5Stage_H_NF:
        return "hazard detection, no forwarding";
    case ProcessorType::RV5Stage_NH_F:
        return "no hazard detection, forwarding";
    case ProcessorType::RV5Stage_H_F:
        return "hazard detection, forwarding";
    default:
        return "single cycle";
    }
}

Suggestion makeSuggestion(const SuggestionRequest &request)
{
    if (!canSuggestFor(request.processor))
    {
        throw std::runtime_error("suggestions are for the pipelined processor models");
    }

    Suggestion suggestion;
    suggestion.plannedLatency =
        request.plannedLatency >= 0
            ? request.plannedLatency
            : (request.memory == MemorySetup::ColdCache ? kColdCacheL2Latency : 0);

    optimizer::OptimizeOptions options;
    options.variant = variantOf(request.processor);
    options.loadExtraLatency = suggestion.plannedLatency;
    const bool detectsHazards = options.variant == optimizer::PipelineVariant::H_NF ||
                                options.variant == optimizer::PipelineVariant::H_F;

    // Without hazard detection a program is only correct with enough padding, so start from the
    // minimally padded program; the suggestion then redoes the padding where it is needed.
    suggestion.baseline = request.source;
    if (!detectsHazards)
    {
        optimizer::OptimizeOptions pad = options;
        pad.padOnly = true;
        suggestion.baseline = optimizer::optimizeSource(request.source, pad).source;
        suggestion.paddingAdded =
            countPaddingLines(suggestion.baseline) - countPaddingLines(request.source);
    }

    options.dropNops = true;
    options.unrollFactor = request.unrollFactor;
    suggestion.result = optimizer::optimizeSource(suggestion.baseline, options);
    for (const optimizer::LoopReport &loop : suggestion.result.loops)
    {
        suggestion.renamedRegisters.insert(suggestion.renamedRegisters.end(),
                                           loop.renamedTo.begin(), loop.renamedTo.end());
    }

    if (request.measure)
    {
        const ProgramRun reference = runProgram(request.source, ProcessorType::RVSS);
        const ProgramRun before = runProgram(suggestion.baseline, request.processor, request.memory);
        const ProgramRun after =
            runProgram(suggestion.result.source, request.processor, request.memory);
        suggestion.measured = true;
        suggestion.baselineCycles = before.cycles;
        suggestion.suggestionCycles = after.cycles;
        suggestion.baselineCorrect = sameResult(reference.state, before.state);
        suggestion.suggestionCorrect =
            sameResult(reference.state, after.state, suggestion.renamedRegisters);
    }
    return suggestion;
}

} // namespace benchmarking
} // namespace Kites
