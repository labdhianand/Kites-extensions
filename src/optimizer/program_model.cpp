/**
 * @file program_model.cpp
 * @brief Implementation of the program model and basic-block builder.
 */
#include "optimizer/program_model.h"

namespace Kites
{
namespace optimizer
{
namespace
{
unsigned sourceLineOf(const AssembledProgram &program, size_t instructionIndex)
{
    const auto it =
        program.instruction_number_line_number_mapping.find(static_cast<unsigned>(instructionIndex));
    if (it != program.instruction_number_line_number_mapping.end())
    {
        return it->second;
    }
    if (instructionIndex < program.intermediate_code.size())
    {
        return program.intermediate_code[instructionIndex].first.line_number;
    }
    return 0;
}

// Groups consecutive instructions that came from the same source line.
std::vector<Unit> buildUnits(const AssembledProgram &program, const std::vector<InstrInfo> &instructions)
{
    std::vector<Unit> units;
    for (size_t i = 0; i < instructions.size(); ++i)
    {
        const unsigned line = sourceLineOf(program, i);
        if (units.empty() || units.back().sourceLine != line)
        {
            Unit unit;
            unit.firstInstruction = i;
            unit.sourceLine = line;
            units.push_back(unit);
        }

        Unit &unit = units.back();
        ++unit.instructionCount;
        unit.readsPc = unit.readsPc || instructions[i].readsPc;
        unit.isBarrier = unit.isBarrier || instructions[i].isBarrier;
        unit.endsBlock = instructions[i].endsBlock; // only the last instruction decides this
    }
    return units;
}

// Marks every instruction index at which a basic block must begin.
std::vector<bool> findLeaders(const AssembledProgram &program,
                              const std::vector<InstrInfo> &instructions)
{
    const size_t instructionCount = instructions.size();
    std::vector<bool> isLeader(instructionCount, false);
    if (instructionCount == 0)
    {
        return isLeader;
    }
    isLeader[0] = true;

    for (size_t i = 0; i < instructionCount; ++i)
    {
        const InstrInfo &info = instructions[i];
        if (info.endsBlock && i + 1 < instructionCount)
        {
            isLeader[i + 1] = true;
        }
        if (info.hasTarget && info.targetPc % 4 == 0 && info.targetPc / 4 < instructionCount)
        {
            isLeader[info.targetPc / 4] = true;
        }
    }

    // A label can be reached indirectly (la + jalr), so every text label starts a block.
    for (const auto &[name, symbol] : program.symbol_table)
    {
        if (!symbol.isData && symbol.address % 4 == 0 && symbol.address / 4 < instructionCount)
        {
            isLeader[symbol.address / 4] = true;
        }
    }
    return isLeader;
}
} // namespace

ProgramModel buildProgramModel(const AssembledProgram &program)
{
    ProgramModel model;

    model.instructions.reserve(program.text_buffer.size());
    for (size_t i = 0; i < program.text_buffer.size(); ++i)
    {
        model.instructions.push_back(decodeInstruction(program.text_buffer[i], i * 4));
    }

    model.units = buildUnits(program, model.instructions);
    const std::vector<bool> isLeader = findLeaders(program, model.instructions);

    for (size_t u = 0; u < model.units.size(); ++u)
    {
        const Unit &unit = model.units[u];

        // A leader that is not at the start of a unit means something can jump into the middle
        // of a pseudo-instruction expansion.
        bool entersMidUnit = false;
        for (size_t k = unit.firstInstruction + 1;
             k < unit.firstInstruction + unit.instructionCount; ++k)
        {
            entersMidUnit = entersMidUnit || isLeader[k];
        }

        if (u == 0 || isLeader[unit.firstInstruction])
        {
            BasicBlock block;
            block.firstUnit = u;
            block.firstInstruction = unit.firstInstruction;
            model.blocks.push_back(block);
        }

        BasicBlock &block = model.blocks.back();
        ++block.unitCount;
        block.instructionCount += unit.instructionCount;
        block.isReorderable = block.isReorderable && !entersMidUnit;
    }

    for (BasicBlock &block : model.blocks)
    {
        const size_t lastInstruction = block.firstInstruction + block.instructionCount - 1;
        block.hasTerminator = model.instructions[lastInstruction].endsBlock;
    }

    return model;
}

} // namespace optimizer
} // namespace Kites
