/**
 * @file liveness.h
 * @brief Which registers can still be read after each basic block.
 *
 * On a pipeline variant without hazard detection, reordering a block can leave a register's last
 * write too close to a successor block that reads it. Only registers that a successor really
 * reads need protecting, which is what this analysis finds.
 */
#pragma once

#include <bitset>
#include <cstddef>
#include <vector>

#include "optimizer/program_model.h"

namespace Kites
{
namespace optimizer
{
/// One bit per register: 32 GPRs followed by 32 FPRs (the same numbering as elsewhere).
using RegisterSet = std::bitset<64>;

struct BlockLiveness
{
    RegisterSet liveIn;  ///< read by the block, or after it, before being written
    RegisterSet liveOut; ///< may be read after the block ends
};

/**
 * @brief Computes live-in and live-out registers for every basic block of the program.
 *
 * Successors are the fall-through block and statically known branch/jump targets. A call (`jal`
 * that writes a link register) also continues at the next block when the callee returns. A jump
 * through a register (`jalr`, including `ret`) has unknown successors, so everything is assumed
 * live after it. Whatever follows the last block is not read, because the program ends there.
 */
[[nodiscard]] std::vector<BlockLiveness> computeLiveness(const ProgramModel &model);

} // namespace optimizer
} // namespace Kites
