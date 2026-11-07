/**
 * @file cost_model.h
 * @brief How many cycles a dependent instruction must wait on each of the four 5-stage pipeline
 * variants.
 *
 * The numbers were measured on the simulator (see the validation tests), not taken from a textbook.
 * Let `distance` be how many issue slots later the consumer is than the producer (an adjacent
 * instruction has distance 1). The consumer reads the producer's result correctly only if
 * distance >= minGap:
 *
 * |                       | producer is ALU/MUL/FP | producer is a load |
 * | no forwarding         |          3             |         3          |
 * | forwarding            |          1             |         2          |
 *
 * The variants differ in who enforces that. With hazard detection (H_*) the hardware stalls the
 * consumer until the gap is met, so a short gap costs cycles. Without it (NH_*) nothing stalls, so
 * a short gap silently produces a wrong result and the programmer has to pad with NOPs.
 *
 * With the simulator's memory-latency model in non-blocking mode (processor/memory_timing.h), a
 * load takes extra cycles and only its consumer waits for them, so a load's gap grows by that
 * amount. `loadExtraLatency` is the latency the scheduler should plan for, typically the cost of a
 * cache miss. The no-hazard variants block on slow loads instead, which no order can hide, so for
 * them it is ignored.
 */
#pragma once

#include "optimizer/instr_info.h"

namespace Kites
{
namespace optimizer
{
enum class PipelineVariant
{
    NH_NF, ///< no hazard detection, no forwarding
    H_NF,  ///< hazard detection (stalls), no forwarding
    NH_F,  ///< no hazard detection, forwarding
    H_F    ///< hazard detection (stalls), forwarding
};

struct CostModel
{
    PipelineVariant variant{PipelineVariant::H_F};

    /// Extra cycles a load is expected to take beyond the normal pipeline (see above).
    int loadExtraLatency{0};

    [[nodiscard]] bool hasForwarding() const
    {
        return variant == PipelineVariant::NH_F || variant == PipelineVariant::H_F;
    }

    /// True if the hardware stalls on a dependency (so a short gap costs cycles). False if it does
    /// not (so a short gap gives a wrong result).
    [[nodiscard]] bool detectsHazards() const
    {
        return variant == PipelineVariant::H_NF || variant == PipelineVariant::H_F;
    }

    /// Extra latency a load's consumer has to wait for: only where the hardware can make it wait.
    [[nodiscard]] int effectiveLoadExtraLatency() const
    {
        return detectsHazards() ? loadExtraLatency : 0;
    }

    /// The largest minGap of any producer on this variant.
    [[nodiscard]] int maxGap() const
    {
        return (hasForwarding() ? 2 : 3) + effectiveLoadExtraLatency();
    }

    /// The smallest distance, in issue slots, at which a consumer sees the result of `producer`.
    [[nodiscard]] int minGap(const InstrInfo &producer) const
    {
        const bool isLoad = producer.klass == InstrClass::Load;
        const int base = hasForwarding() ? (isLoad ? 2 : 1) : 3;
        return base + (isLoad ? effectiveLoadExtraLatency() : 0);
    }
};

} // namespace optimizer
} // namespace Kites
