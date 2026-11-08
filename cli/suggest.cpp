/**
 * @file suggest.cpp
 * @brief Implementation of `kites-cli --suggest`.
 */
#include "suggest.h"

#include "benchmarking/suggestion.h"
#include "optimizer/loop_unroller.h"
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
std::string percentChange(unsigned before, unsigned after)
{
    std::ostringstream out;
    const double change = 100.0 * (static_cast<double>(after) - before) / before;
    out << std::fixed << std::setprecision(1) << (change > 0 ? "+" : "") << change << "%";
    return out.str();
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
            if (loop.foldedInstructions > 0)
            {
                std::cout << ", " << loop.foldedInstructions << " repeated address instructions removed";
            }
            std::cout << ")\n";
            if (!loop.changedDeadRegisters.empty())
            {
                std::cout << "    not read after the loop, so they now end differently:";
                for (unsigned reg : loop.changedDeadRegisters)
                {
                    std::cout << " x" << reg;
                }
                std::cout << "\n";
            }
            std::string saved;
            for (unsigned reg : loop.renamedTo)
            {
                if (optimizer::isCalleeSaved(reg))
                {
                    saved += (saved.empty() ? "x" : ", x") + std::to_string(reg);
                }
            }
            if (!saved.empty())
            {
                std::cout << "    note: no temporary register was free, so callee-saved " << saved
                          << " is used; inside a function, save and restore it\n";
            }
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
    if (!benchmarking::canSuggestFor(options.processor))
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

    benchmarking::SuggestionRequest request;
    request.source = buffer.str();
    request.processor = options.processor;
    request.memory =
        options.coldCache ? benchmarking::MemorySetup::ColdCache : benchmarking::MemorySetup::Ideal;
    request.unrollFactor = options.unrollFactor;
    request.allowDeadRegisterChanges = options.allowDeadRegisterChanges;
    request.plannedLatency = options.plannedLatency;
    request.measure = options.verify;

    benchmarking::Suggestion suggestion;
    try
    {
        suggestion = benchmarking::makeSuggestion(request);
    }
    catch (const std::exception &error)
    {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
    const optimizer::OptimizeResult &result = suggestion.result;

    const std::string name = std::filesystem::path(options.assemblyFile).filename().string();
    std::cout << "Reordering suggestion for " << name << " on " << options.processorName << " ("
              << benchmarking::describe(options.processor) << ")\n";
    std::cout << "Planned load latency: +" << suggestion.plannedLatency << " cycles";
    if (options.unrollFactor > 1)
    {
        std::cout << ", loops unrolled up to x" << options.unrollFactor;
    }
    std::cout << "\n";
    if (suggestion.baseline != request.source)
    {
        std::cout << "\nThis pipeline does not stall on hazards, so the program was first padded to "
                     "compute the right result ("
                  << suggestion.paddingAdded << " NOPs added). The suggestion is compared with "
                  << "that padded program.\n";
    }

    printLoops(result.loops);
    printBlocks(result);

    if (!result.changed)
    {
        std::cout << "\nNo better order found"
                  << (suggestion.baseline != request.source ? "; only the padding is needed.\n"
                                                             : ".\n");
    }
    if (result.source != request.source)
    {
        std::cout << "\n" << optimizer::unifiedDiff(request.source, result.source, name, "suggested");
    }

    int exitCode = 0;
    if (suggestion.measured)
    {
        std::cout << "\nMeasured on " << options.processorName << " with "
                  << benchmarking::describe(request.memory) << ":\n";
        std::cout << "  "
                  << (suggestion.baseline != request.source ? "padded original " : "original        ")
                  << std::setw(7) << suggestion.baselineCycles << " cycles"
                  << (suggestion.baselineCorrect ? "" : "   (computes a different result!)") << "\n";
        std::cout << "  suggested       " << std::setw(7) << suggestion.suggestionCycles
                  << " cycles   "
                  << percentChange(suggestion.baselineCycles, suggestion.suggestionCycles) << "\n";
        std::cout << "  result          "
                  << (suggestion.suggestionCorrect
                          ? "identical to the original on the single-cycle processor"
                          : "DIFFERENT from the original")
                  << "\n";
        if (!suggestion.renamedRegisters.empty())
        {
            std::cout << "                  (registers the original never used are not compared)\n";
        }
        if (!suggestion.changedDeadRegisters.empty())
        {
            std::cout << "                  (nor registers nothing reads after their loop)\n";
        }
        if (!suggestion.suggestionCorrect)
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
