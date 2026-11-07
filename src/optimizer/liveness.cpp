/**
 * @file liveness.cpp
 * @brief Implementation of the block-level liveness analysis.
 */
#include "optimizer/liveness.h"

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
} // namespace

std::vector<BlockLiveness> computeLiveness(const ProgramModel &model)
{
    const size_t blockCount = model.blocks.size();
    std::vector<BlockLiveness> liveness(blockCount);
    if (blockCount == 0)
    {
        return liveness;
    }

    std::vector<size_t> blockOfInstruction(model.instructions.size(), 0);
    for (size_t b = 0; b < blockCount; ++b)
    {
        for (size_t k = model.blocks[b].firstInstruction;
             k < model.blocks[b].firstInstruction + model.blocks[b].instructionCount; ++k)
        {
            blockOfInstruction[k] = b;
        }
    }

    std::vector<UseDef> useDef;
    std::vector<std::vector<size_t>> successors(blockCount);
    std::vector<bool> successorsUnknown(blockCount, false);
    for (size_t b = 0; b < blockCount; ++b)
    {
        const BasicBlock &block = model.blocks[b];
        useDef.push_back(useDefOf(model, block));

        const InstrInfo &last =
            model.instructions[block.firstInstruction + block.instructionCount - 1];
        const bool hasNext = b + 1 < blockCount;
        auto addTarget = [&]()
        {
            if (last.hasTarget && last.targetPc % 4 == 0 &&
                last.targetPc / 4 < model.instructions.size())
            {
                successors[b].push_back(blockOfInstruction[last.targetPc / 4]);
            }
        };

        switch (last.klass)
        {
        case InstrClass::Branch:
            addTarget();
            if (hasNext)
            {
                successors[b].push_back(b + 1);
            }
            break;
        case InstrClass::Jal:
            addTarget();
            // A call (it writes a link register) continues at the next block once the callee
            // returns; a plain jump does not.
            if (last.dest.isValid() && hasNext)
            {
                successors[b].push_back(b + 1);
            }
            break;
        case InstrClass::Jalr:
            successorsUnknown[b] = true;
            break;
        default:
            if (hasNext)
            {
                successors[b].push_back(b + 1);
            }
            break;
        }
    }

    // Standard backward dataflow, repeated until nothing changes.
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (size_t b = blockCount; b-- > 0;)
        {
            RegisterSet liveOut;
            if (successorsUnknown[b])
            {
                liveOut.set();
            }
            for (size_t successor : successors[b])
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
