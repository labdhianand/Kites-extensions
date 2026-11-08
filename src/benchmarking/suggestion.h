/**
 * @file suggestion.h
 * @brief One complete reordering suggestion for a program: what the optimizer suggests, and how
 * it measures on the simulator. Shared by `kites-cli --suggest` and the GUI's Optimizer tab.
 */
#pragma once

#include <string>
#include <vector>

#include "benchmarking/program_runner.h"
#include "optimizer/optimizer.h"
#include "processor/processor_types.h"

namespace Kites
{
namespace benchmarking
{
struct SuggestionRequest
{
    std::string source;
    ProcessorType processor{ProcessorType::RV5Stage_H_F};
    MemorySetup memory{MemorySetup::Ideal};

    /// Unroll counted loops by up to this factor (1 = no unrolling).
    int unrollFactor{1};

    /// Only memory and the registers read after each loop must come out the same, which lets
    /// unrolling drop repeated address arithmetic. Off: every register must match.
    bool allowDeadRegisterChanges{false};

    /// Extra cycles per load to plan for; -1 picks a default for the memory setup.
    int plannedLatency{-1};

    /// Run the original and the suggestion on the simulator.
    bool measure{true};
};

struct Suggestion
{
    /// The program the suggestion is compared with: the source itself, or on a pipeline without
    /// hazard detection the source padded with just the NOPs it needs to be correct.
    std::string baseline;
    int paddingAdded{0}; ///< NOPs added to make the baseline

    optimizer::OptimizeResult result;
    int plannedLatency{0};

    /// Registers the suggestion uses that the original never did (from unrolling).
    std::vector<unsigned> renamedRegisters;
    /// Registers that end with a different value because nothing reads them after their loop
    /// (only with allowDeadRegisterChanges). Like renamedRegisters, they are not compared.
    std::vector<unsigned> changedDeadRegisters;

    bool measured{false};
    unsigned baselineCycles{0};
    unsigned suggestionCycles{0};
    bool baselineCorrect{false};   ///< the baseline matches the single-cycle processor
    bool suggestionCorrect{false}; ///< the suggestion matches it too
};

/// True for the pipelined processor models, the only ones a suggestion makes sense for.
[[nodiscard]] bool canSuggestFor(ProcessorType processor);

/// A short description of a processor model, e.g. "hazard detection, forwarding".
[[nodiscard]] std::string describe(ProcessorType processor);

/**
 * @brief Computes a suggestion and, if asked, measures it.
 * @throws std::runtime_error if the program does not assemble or cannot be padded, or if
 * `processor` is not a pipelined model.
 */
[[nodiscard]] Suggestion makeSuggestion(const SuggestionRequest &request);

} // namespace benchmarking
} // namespace Kites
