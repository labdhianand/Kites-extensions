/**
 * @file memory_timing.h
 * @brief Opt-in model of how long a load takes, depending on which level of the memory hierarchy
 * serves it.
 *
 * Without it every memory access fits in the one-cycle MEM stage, so a cache miss costs nothing.
 * With it a load costs extra cycles: one value when it hits in L1, another when L1 misses but L2
 * hits, and another when it has to go to main memory. Stores are assumed to go through a write
 * buffer and never stall.
 *
 * The extra cycles are spent in one of two ways:
 *  - blocking: the whole pipeline waits for the load, as in the simplest in-order design. No
 *    instruction order can hide the delay.
 *  - non-blocking: the pipeline keeps issuing, and only an instruction that reads the loaded
 *    register waits for it (a scoreboard). This is what lets reordering hide memory delays. It
 *    needs hardware that can make an instruction wait, so the pipeline variants without hazard
 *    detection always block.
 *
 * Only the 5-stage pipelines use this model.
 */
#pragma once

namespace Kites
{
struct MemoryTiming
{
    bool enabled{false};
    bool nonBlocking{false};

    /// Extra cycles on top of the normal one-cycle MEM stage, by the level that served the load.
    unsigned l1HitCycles{0};
    unsigned l2HitCycles{0};
    unsigned memoryCycles{0};
};
} // namespace Kites
