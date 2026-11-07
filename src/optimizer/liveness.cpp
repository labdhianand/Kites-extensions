/**
 * @file liveness.cpp
 * @brief Implementation of the block-level control flow and liveness analysis.
 */
#include "optimizer/liveness.h"

#include <algorithm>

namespace Kites
{
namespace optimizer
{
namespace
{
size_t registerSlot(RegRef reg)
{
    return (reg.file == RegFile::Fpr ? 32 : 0) + reg.index;
}

struct UseDef
{
    RegisterSet use; ///< read before being written inside the block
    RegisterSet def; ///< written inside the block
};

UseDef useDefOf(const ProgramModel &model, const BasicBlock &block)
{
    UseDef result;
    for (size_t k = block.firstInstruction; k < block.firstInstruction + block.instructionCount;
         ++k)
    {
        const InstrInfo &info = model.instructions[k];
        for (size_t s = 0; s < info.sourceCount; ++s)
        {
            const size_t reg = registerSlot(info.sources[s]);
            if (!result.def[reg])
            {
                result.use.set(reg);
            }
        }
        if (info.dest.isValid())
        {
            result.def.set(registerSlot(info.dest));
        }
    }
    return result;
}

const InstrInfo &lastInstructionOf(const ProgramModel &model, const BasicBlock &block)
{
    return model.instructions[block.firstInstruction + block.instructionCount - 1];
}
} // namespace

ControlFlow computeControlFlow(const ProgramModel &model)
{
    const size_t blockCount = model.blocks.size();
    ControlFlow flow;
    flow.successors.resize(blockCount);
    flow.predecessors.resize(blockCount);
    flow.hasUnknownSuccessors.assign(blockCount, false);
    flow.hasUnknownPredecessors.assign(blockCount, false);

    std::vector<size_t> blockOfInstruction(model.instructions.size(), 0);
    for (size_t b = 0; b < blockCount; ++b)
    {
        for (size_t k = model.blocks[b].firstInstruction;
             k < model.blocks[b].firstInstruction + model.blocks[b].instructionCount; ++k)
        {
            blockOfInstruction[k] = b;
        }
    }

    // `la` of a data label also uses auipc, but its target is far from the code (data starts at
    // 0x10000000). Only an auipc that lands near the code can produce a jump target; the 4 KiB
    // margin covers the low 12 bits added by the following instruction.
    const int64_t codeBytes = static_cast<int64_t>(model.instructions.size()) * 4;
    bool computesCodeAddresses = false;
    for (const InstrInfo &info : model.instructions)
    {
        if (info.readsPc)
        {
            const int64_t target = static_cast<int64_t>(info.pc) + info.imm;
            computesCodeAddresses =
                computesCodeAddresses || (target > -4096 && target < codeBytes + 4096);
        }
    }

    for (size_t b = 0; b < blockCount; ++b)
    {
        const InstrInfo &last = lastInstructionOf(model, model.blocks[b]);
        const bool hasNext = b + 1 < blockCount;
        auto addSuccessor = [&](size_t successor)
        {
            if (std::find(flow.successors[b].begin(), flow.successors[b].end(), successor) ==
                flow.successors[b].end())
            {
                flow.successors[b].push_back(successor);
            }
        };
        auto addTarget = [&]()
        {
            if (last.hasTarget && last.targetPc % 4 == 0 &&
                last.targetPc / 4 < model.instructions.size())
            {
                addSuccessor(blockOfInstruction[last.targetPc / 4]);
            }
        };

        switch (last.klass)
        {
        case InstrClass::Branch:
            addTarget();
            if (hasNext)
            {
                addSuccessor(b + 1);
            }
            break;
        case InstrClass::Jal:
            addTarget();
            // A call (it writes a link register) continues at the next block once the callee
            // returns; a plain jump does not. The return itself is an unknown jump, so the
            // next block is also entered from somewhere unknown.
            if (last.dest.isValid() && hasNext)
            {
                addSuccessor(b + 1);
                flow.hasUnknownPredecessors[b + 1] = true;
            }
            break;
        case InstrClass::Jalr:
            flow.hasUnknownSuccessors[b] = true;
            break;
        default:
            if (hasNext)
            {
                addSuccessor(b + 1);
            }
            break;
        }
    }

    for (size_t b = 0; b < blockCount; ++b)
    {
        for (size_t successor : flow.successors[b])
        {
            flow.predecessors[successor].push_back(b);
        }
    }

    // With auipc a program can compute a code address and jump there, so any block may be entered
    // from somewhere unknown.
    if (computesCodeAddresses)
    {
        flow.hasUnknownPredecessors.assign(blockCount, true);
    }
    return flow;
}

std::vector<BlockLiveness> computeLiveness(const ProgramModel &model)
{
    const size_t blockCount = model.blocks.size();
    std::vector<BlockLiveness> liveness(blockCount);
    if (blockCount == 0)
    {
        return liveness;
    }

    const ControlFlow flow = computeControlFlow(model);
    std::vector<UseDef> useDef;
    for (const BasicBlock &block : model.blocks)
    {
        useDef.push_back(useDefOf(model, block));
    }

    // Standard backward dataflow, repeated until nothing changes.
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (size_t b = blockCount; b-- > 0;)
        {
            RegisterSet liveOut;
            if (flow.hasUnknownSuccessors[b])
            {
                liveOut.set();
            }
            for (size_t successor : flow.successors[b])
            {
                liveOut |= liveness[successor].liveIn;
            }
            const RegisterSet liveIn = useDef[b].use | (liveOut & ~useDef[b].def);

            if (liveOut != liveness[b].liveOut || liveIn != liveness[b].liveIn)
            {
                liveness[b].liveOut = liveOut;
                liveness[b].liveIn = liveIn;
                changed = true;
            }
        }
    }
    return liveness;
}

} // namespace optimizer
} // namespace Kites
