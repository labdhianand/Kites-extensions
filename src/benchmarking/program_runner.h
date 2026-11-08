/**
 * @file program_runner.h
 * @brief Runs an assembly program to completion on any processor model and reports what it left
 * behind, for the optimizer's tools (kites-cli --suggest, kites-bench).
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "processor/processor_types.h"

namespace Kites
{
namespace benchmarking
{
/// The registers and the first bytes of the data section after a run.
struct ProgramState
{
    std::array<uint64_t, 32> gprs{};
    std::vector<uint32_t> data;

    bool operator==(const ProgramState &other) const = default;
};

struct ProgramRun
{
    ProgramState state;
    unsigned cycles{0};
};

/// How memory behaves during a run.
enum class MemorySetup
{
    /// Every access fits in the one-cycle MEM stage (the simulator's default).
    Ideal,
    /// A small cache with load latencies: L1 4 sets x 2 ways x 16 B (128 B), L2 32 sets x 4 ways x
    /// 16 B (2 KB), extra cycles per load L1 hit +0, L2 hit +4, main memory +20. Loads are
    /// non-blocking where the hardware can wait for them (hazard detection) and blocking elsewhere.
    ColdCache
};

/// The extra latency of an L2 hit under MemorySetup::ColdCache, a sensible amount for the
/// optimizer to plan for.
inline constexpr int kColdCacheL2Latency = 4;

/// A one-line description of a memory setup, for reports.
[[nodiscard]] std::string describe(MemorySetup setup);

/**
 * @brief Assembles `source` and runs it to completion on a fresh processor of the given type.
 * @throws std::runtime_error if the source does not assemble.
 */
[[nodiscard]] ProgramRun runProgram(const std::string &source, ProcessorType type,
                                    MemorySetup memory = MemorySetup::Ideal);

/// True if the two states match, ignoring the given registers (ones an optimization started
/// using, whose final values the original program never defined).
[[nodiscard]] bool sameResult(const ProgramState &expected, const ProgramState &actual,
                              const std::vector<unsigned> &ignoredRegisters = {});

} // namespace benchmarking
} // namespace Kites
