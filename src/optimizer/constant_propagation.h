/**
 * @file constant_propagation.h
 * @brief Which integer registers hold a known constant at the start and end of each block.
 *
 * A register is known at a block's entry only if every way into the block leaves the same
 * constant in it. Values the analysis cannot compute (loads, multiplies, anything coming from an
 * unknown jump) are unknown. Used to work out how many times a loop runs.
 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "optimizer/liveness.h"
#include "optimizer/program_model.h"

namespace Kites
{
namespace optimizer
{
/// The known value of each integer register, or nullopt. x0 is always 0.
using ConstantRegisters = std::array<std::optional<uint64_t>, 32>;

struct BlockConstants
{
    bool reachable{false};
    ConstantRegisters atEntry{};
    ConstantRegisters atExit{};
};

[[nodiscard]] std::vector<BlockConstants> propagateConstants(const ProgramModel &model,
                                                             const ControlFlow &flow);

/// Runs `block` on `entry` and returns the registers at its end.
[[nodiscard]] ConstantRegisters runBlockOnConstants(const ProgramModel &model,
                                                    const BasicBlock &block,
                                                    ConstantRegisters entry);

} // namespace optimizer
} // namespace Kites
