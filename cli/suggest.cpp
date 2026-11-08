/**
 * @file suggest.cpp
 * @brief Implementation of `kites-cli --suggest`.
 */
#include "suggest.h"

#include "benchmarking/program_runner.h"
#include "optimizer/optimizer.h"
#include "optimizer/source_text.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace Kites
{
namespace
{
using optimizer::PipelineVariant;

struct VariantDescription
{
    PipelineVariant variant;
    const char *description;
};

VariantDescription describeVariant(ProcessorType type)
{
    switch (type)
    {
    case ProcessorType::RV5Stage_NH_NF:
        return {PipelineVariant::NH_NF, "no hazard detection, no forwarding"};
    case ProcessorType::RV5Stage_H_NF:
        return {PipelineVariant::H_NF, "hazard detection, no forwarding"};
    case ProcessorType::RV5Stage_NH_F:
        return {PipelineVariant::NH_F, "no hazard detection, forwarding"};
    default:
        return {PipelineVariant::H_F, "hazard detection, forwarding"};
    }
}

std::string percentChange(unsigned before, unsigned after)
{
    std::ostringstream out;
    const double change = 100.0 * (static_cast<double>(after) - before) / before;
    out << std::fixed << std::setprecision(1) << (change > 0 ? "+" : "") << change << "%";
    return out.str();
}

int countLines(const std::string &text, const std::string &needle)
{
    int count = 0;
    for (const std::string &line : optimizer::splitSource(text).lines)
    {
        count += line.find(needle) != std::string::npos ? 1 : 0;
    }
    return count;
}

void printLoops(const std::vector<optimizer::LoopReport> &loops)
{
    if (loops.empty())
    {
        return;
    }
    std::cout << "\nLoops:\n";
    for (const optimizer::LoopReport &loop : loops)
    {
        std::cout << "  lines " << loop.firstLine << "-" << loop.lastLine << ": ";
        if (loop.unrolled)
        {
            std::cout << "unrolled x" << loop.factor << " (" << loop.tripCount << " iterations";
            if (!loop.renamedTo.empty())
            {
                std::cout << ", " << loop.renamedTo.size() << " registers renamed";
            }
            std::cout << ")\n";
        }
        else
        {
            std::cout << "left as is: " << loop.reason << "\n";
        }
    }
}

void printBlocks(const optimizer::OptimizeResult &result)
{
    int changed = 0;
    for (const optimizer::BlockSummary &block : result.blocks)
    {
        changed += block.changed ? 1 : 0;
    }
    std::cout << "\nBasic blocks: " << result.blocks.size() << ", reordered: " << changed << "\n";
    for (const optimizer::BlockSummary &block : result.blocks)
    {
        if (block.changed)
        {
            std::cout << "  lines " << block.firstLine << "-" << block.lastLine << ": "
                      << block.originalSlots << " -> " << block.optimizedSlots
                      << " slots per pass\n";
        }
        else if (!block.reason.empty())
        {
            std::cout << "  lines " << block.firstLine << "-" << block.lastLine
                      << ": left as is: " << block.reason << "\n";
        }
    }
    if (!result.loops.empty())
    {
        std::cout << "  (line numbers refer to the program after unrolling)\n";
    }
}
} // namespace

int RunSuggest(const SuggestOptions &options)
{
    if (options.processor == ProcessorType::RVSS || options.processor == ProcessorType::ProcessorTypeCount)
    {
        std::cerr << "Error: --suggest needs a pipelined processor model "
                     "(--vm rv5-nh-nf | rv5-h-nf | rv5-nh-f | rv5-h-f).\n";
        return 1;
    }

    std::ifstream file(options.assemblyFile);
    if (!file)
    {
        std::cerr << "Error: cannot read '" << options.assemblyFile << "'.\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string source = buffer.str();

    const VariantDescription variant = describeVariant(options.processor);
    const benchmarking::MemorySetup memory =
        options.coldCache ? benchmarking::MemorySetup::ColdCache : benchmarking::MemorySetup::Ideal;
    const int plannedLatency =
        options.plannedLatency >= 0 ? options.plannedLatency
                                    : (options.coldCache ? benchmarking::kColdCacheL2Latency : 0);
    const bool detectsHazards =
        variant.variant == PipelineVariant::H_NF || variant.variant == PipelineVariant::H_F;

    optimizer::OptimizeOptions optimizeOptions;
    optimizeOptions.variant = variant.variant;
    optimizeOptions.loadExtraLatency = plannedLatency;

    std::string baseline = source;
    optimizer::OptimizeResult result;
    try
    {
        // Without hazard detection a program is only correct with enough padding, so start from
        // the minimally padded program; the suggestion then redoes the padding where it is needed.
        if (!detectsHazards)
        {
            optimizer::OptimizeOptions pad = optimizeOptions;
            pad.padOnly = true;
            baseline = optimizer::optimizeSource(source, pad).source;
        }
        optimizeOptions.dropNops = true;
        optimizeOptions.unrollFactor = options.unrollFactor;
        result = optimizer::optimizeSource(baseline, optimizeOptions);
    }
    catch (const std::exception &error)
    {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }

    const std::string name = std::filesystem::path(options.assemblyFile).filename().string();
    std::cout << "Reordering suggestion for " << name << " on " << options.processorName << " ("
              << variant.description << ")\n";
    std::cout << "Planned load latency: +" << plannedLatency << " cycles";
    if (options.unrollFactor > 1)
    {
        std::cout << ", loops unrolled up to x" << options.unrollFactor;
    }
    std::cout << "\n";
    if (baseline != source)
    {
        std::cout << "\nThis pipeline does not stall on hazards, so the program was first padded to "
                     "compute the right result ("
                  << countLines(baseline, "# padding") << " NOPs added). The suggestion is "
                  << "compared with that padded program.\n";
    }

    printLoops(result.loops);
    printBlocks(result);

    if (!result.changed)
    {
        std::cout << "\nNo better order found"
                  << (baseline != source ? "; only the padding is needed.\n" : ".\n");
    }
    if (result.source != source)
    {
        std::cout << "\n"
                  << optimizer::unifiedDiff(source, result.source, name, "suggested");
    }

    int exitCode = 0;
    if (options.verify)
    {
        std::vector<unsigned> renamed;
        for (const optimizer::LoopReport &loop : result.loops)
        {
            renamed.insert(renamed.end(), loop.renamedTo.begin(), loop.renamedTo.end());
        }
        const benchmarking::ProgramRun reference =
            benchmarking::runProgram(source, ProcessorType::RVSS);
        const benchmarking::ProgramRun before =
            benchmarking::runProgram(baseline, options.processor, memory);
        const benchmarking::ProgramRun after =
            benchmarking::runProgram(result.source, options.processor, memory);
        const bool beforeCorrect = benchmarking::sameResult(reference.state, before.state);
        const bool afterCorrect = benchmarking::sameResult(reference.state, after.state, renamed);

        std::cout << "\nMeasured on " << options.processorName << " with "
                  << benchmarking::describe(memory) << ":\n";
        std::cout << "  " << (baseline != source ? "padded original " : "original        ")
                  << std::setw(7) << before.cycles << " cycles"
                  << (beforeCorrect ? "" : "   (computes a different result!)") << "\n";
        std::cout << "  suggested       " << std::setw(7) << after.cycles << " cycles   "
                  << percentChange(before.cycles, after.cycles) << "\n";
        std::cout << "  result          "
                  << (afterCorrect ? "identical to the original on the single-cycle processor"
                                   : "DIFFERENT from the original")
                  << "\n";
        if (!renamed.empty())
        {
            std::cout << "                  (registers the original never used are not compared)\n";
        }
        if (!afterCorrect)
        {
            exitCode = 2;
        }
    }

    if (!options.outputFile.empty())
    {
        std::ofstream out(options.outputFile);
        if (!out)
        {
            std::cerr << "Error: cannot write '" << options.outputFile << "'.\n";
            return 1;
        }
        out << result.source;
        std::cout << "\nSuggested program written to " << options.outputFile << "\n";
    }
    return exitCode;
}
} // namespace Kites
