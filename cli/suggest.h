/**
 * @file suggest.h
 * @brief `kites-cli <file> --suggest`: a reordering suggestion for a pipeline, with a diff and
 * measured cycles.
 */
#pragma once

#include <string>

#include "processor/processor_types.h"

namespace Kites
{
struct SuggestOptions
{
    std::string assemblyFile;
    ProcessorType processor{ProcessorType::RV5Stage_H_F};
    std::string processorName{"rv5-h-f"};

    /// Unroll counted loops by up to this factor (1 = no unrolling).
    int unrollFactor{1};

    /// Extra cycles per load to plan for; -1 picks a default for the memory setup.
    int plannedLatency{-1};

    /// Measure with the cache-latency model instead of ideal memory.
    bool coldCache{false};

    /// Run the original and the suggestion on the simulator and compare them.
    bool verify{true};

    /// If set, the suggested program is written here.
    std::string outputFile;
};

/**
 * @brief Prints the suggestion report to stdout.
 * @return 0 on success, 1 if the program cannot be read or assembled or the options are invalid,
 * 2 if the suggestion was verified and computes a different result (which should never happen).
 */
int RunSuggest(const SuggestOptions &options);
} // namespace Kites
