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

    /// The largest minGap of any producer on this variant.
    [[nodiscard]] int maxGap() const
    {
        return hasForwarding() ? 2 : 3;
    }

    /// The smallest distance, in issue slots, at which a consumer sees the result of `producer`.
    [[nodiscard]] int minGap(const InstrInfo &producer) const
    {
        if (!hasForwarding())
        {
            return 3;
        }
        return producer.klass == InstrClass::Load ? 2 : 1;
    }
};

} // namespace optimizer
} // namespace Kites
