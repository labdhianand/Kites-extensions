#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "assembler/assembler.h"
#include "optimizer/program_model.h"

using namespace Kites;
using namespace Kites::optimizer;

namespace
{
ProgramModel modelOf(const std::string &source)
{
    std::istringstream stream(source);
    return buildProgramModel(assemble(stream));
}
} // namespace

TEST(OptimizerProgramModel, StraightLineCodeIsOneBlock)
{
    const ProgramModel model = modelOf(R"(
        add x3, x1, x2
        add x4, x3, x2
        add x5, x4, x2
    )");
    ASSERT_EQ(model.blocks.size(), 1u);
    EXPECT_EQ(model.units.size(), 3u);
    EXPECT_EQ(model.blocks[0].instructionCount, 3u);
    EXPECT_FALSE(model.blocks[0].hasTerminator);
    EXPECT_TRUE(model.blocks[0].isReorderable);
}

TEST(OptimizerProgramModel, InstructionAddressesMatchPosition)
{
    const ProgramModel model = modelOf(R"(
        add x3, x1, x2
        add x4, x3, x2
    )");
    ASSERT_EQ(model.instructions.size(), 2u);
    EXPECT_EQ(model.instructions[0].pc, 0u);
    EXPECT_EQ(model.instructions[1].pc, 4u);
}

TEST(OptimizerProgramModel, BranchEndsBlockAndItsTargetStartsOne)
{
    const ProgramModel model = modelOf(R"(
        beq x1, x2, skip
        add x3, x1, x2
    skip:
        add x4, x1, x2
    )");
    ASSERT_EQ(model.blocks.size(), 3u);
    EXPECT_EQ(model.blocks[0].instructionCount, 1u);
    EXPECT_TRUE(model.blocks[0].hasTerminator);
    EXPECT_EQ(model.blocks[1].instructionCount, 1u);
    EXPECT_FALSE(model.blocks[1].hasTerminator);
    EXPECT_EQ(model.blocks[2].firstInstruction, 2u);
}

TEST(OptimizerProgramModel, LabelStartsABlockEvenWithoutAnyBranchToIt)
{
    const ProgramModel model = modelOf(R"(
        add x1, x1, x1
    middle:
        add x2, x2, x2
        add x3, x3, x3
    )");
    ASSERT_EQ(model.blocks.size(), 2u);
    EXPECT_EQ(model.blocks[0].instructionCount, 1u);
    EXPECT_EQ(model.blocks[1].instructionCount, 2u);
}

TEST(OptimizerProgramModel, LoopIsOneBlockEndingInABackwardBranch)
{
    const ProgramModel model = modelOf(R"(
    loop:
        addi x1, x1, -1
        add  x2, x2, x1
        bne  x1, x0, loop
    )");
    ASSERT_EQ(model.blocks.size(), 1u);
    EXPECT_EQ(model.blocks[0].instructionCount, 3u);
    EXPECT_TRUE(model.blocks[0].hasTerminator);
}

TEST(OptimizerProgramModel, EcallEndsItsBlock)
{
    const ProgramModel model = modelOf(R"(
        addi a7, x0, 10
        ecall
        addi x1, x0, 1
    )");
    ASSERT_EQ(model.blocks.size(), 2u);
    EXPECT_EQ(model.blocks[0].instructionCount, 2u);
    EXPECT_TRUE(model.blocks[0].hasTerminator);
    EXPECT_TRUE(model.units[1].isBarrier);
}

TEST(OptimizerProgramModel, PseudoInstructionExpansionIsOneUnit)
{
    const ProgramModel model = modelOf(R"(
        li x5, 100000
    )");
    EXPECT_GT(model.instructions.size(), 1u);
    ASSERT_EQ(model.units.size(), 1u);
    EXPECT_EQ(model.units[0].instructionCount, model.instructions.size());
}

TEST(OptimizerProgramModel, LaIsAUnitThatReadsItsOwnAddress)
{
    const ProgramModel model = modelOf(R"(
    .data
    value: .word 5
    .text
        la x5, value
        add x6, x5, x5
    )");
    ASSERT_EQ(model.units.size(), 2u);
    EXPECT_EQ(model.units[0].instructionCount, 2u);
    EXPECT_TRUE(model.units[0].readsPc);
    EXPECT_FALSE(model.units[1].readsPc);
}

TEST(OptimizerProgramModel, UnitsRecordOneBasedSourceLines)
{
    const ProgramModel model = modelOf("\n        add x1, x1, x1\n\n        add x2, x2, x2\n");
    ASSERT_EQ(model.units.size(), 2u);
    EXPECT_EQ(model.units[0].sourceLine, 2u);
    EXPECT_EQ(model.units[1].sourceLine, 4u);
}

TEST(OptimizerProgramModel, EmptyProgramHasNoBlocks)
{
    AssembledProgram program;
    const ProgramModel model = buildProgramModel(program);
    EXPECT_TRUE(model.instructions.empty());
    EXPECT_TRUE(model.units.empty());
    EXPECT_TRUE(model.blocks.empty());
}

// ---------------------------------------------------------------------------
// Structural invariants that must hold for every program.
// ---------------------------------------------------------------------------
namespace
{
void expectInvariants(const AssembledProgram &program, const ProgramModel &model,
                      const std::string &label)
{
    const size_t instructionCount = model.instructions.size();

    // Units tile the instruction list exactly.
    size_t next = 0;
    for (const Unit &unit : model.units)
    {
        EXPECT_EQ(unit.firstInstruction, next) << label;
        EXPECT_GT(unit.instructionCount, 0u) << label;
        next += unit.instructionCount;
    }
    EXPECT_EQ(next, instructionCount) << label;

    // Blocks tile the instruction list exactly, and agree with the unit tiling.
    next = 0;
    size_t nextUnit = 0;
    for (const BasicBlock &block : model.blocks)
    {
        EXPECT_EQ(block.firstInstruction, next) << label;
        EXPECT_EQ(block.firstUnit, nextUnit) << label;
        EXPECT_EQ(model.units[block.firstUnit].firstInstruction, block.firstInstruction) << label;
        next += block.instructionCount;
        nextUnit += block.unitCount;

        // Only the last instruction of a block may end it.
        for (size_t i = block.firstInstruction;
             i + 1 < block.firstInstruction + block.instructionCount; ++i)
        {
            EXPECT_FALSE(model.instructions[i].endsBlock)
                << label << ": block-ending instruction " << i << " is not last in its block";
        }
    }
    EXPECT_EQ(next, instructionCount) << label;
    EXPECT_EQ(nextUnit, model.units.size()) << label;

    auto isBlockStart = [&](size_t instructionIndex)
    {
        for (const BasicBlock &block : model.blocks)
        {
            if (block.firstInstruction == instructionIndex)
            {
                return true;
            }
        }
        return false;
    };

    // Every static branch/jump target begins a block, so nothing can enter a block midway.
    for (const InstrInfo &info : model.instructions)
    {
        if (info.hasTarget && info.targetPc % 4 == 0 && info.targetPc / 4 < instructionCount)
        {
            EXPECT_TRUE(isBlockStart(info.targetPc / 4))
                << label << ": target of instruction at 0x" << std::hex << info.pc;
        }
    }

    // So does every text label.
    for (const auto &[name, symbol] : program.symbol_table)
    {
        if (!symbol.isData && symbol.address % 4 == 0 && symbol.address / 4 < instructionCount)
        {
            EXPECT_TRUE(isBlockStart(symbol.address / 4)) << label << ": label " << name;
        }
    }
}
} // namespace

TEST(OptimizerProgramModelInvariants, HoldForAllExamplePrograms)
{
    size_t programCount = 0;
    size_t blockCount = 0;

    for (const char *directory : {"../examples", "../examples/fd-extension"})
    {
        if (!std::filesystem::exists(directory))
        {
            continue;
        }
        for (const auto &entry : std::filesystem::directory_iterator(directory))
        {
            if (entry.path().extension() != ".s")
            {
                continue;
            }
            std::ifstream file(entry.path());
            const AssembledProgram program = assemble(file);
            if (program.text_buffer.empty())
            {
                continue;
            }
            const ProgramModel model = buildProgramModel(program);
            expectInvariants(program, model, entry.path().filename().string());
            ++programCount;
            blockCount += model.blocks.size();
        }
    }

    EXPECT_GT(programCount, 10u);
    EXPECT_GT(blockCount, programCount); // at least some programs have several blocks
}
