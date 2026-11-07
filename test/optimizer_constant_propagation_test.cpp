#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "optimizer/constant_propagation.h"
#include "optimizer/liveness.h"
#include "optimizer_test_support.h"

using namespace Kites;
using namespace Kites::optimizer;
using namespace optimizer_test;

namespace
{
struct Analysis
{
    ProgramModel model;
    ControlFlow flow;
    std::vector<BlockConstants> constants;
};

Analysis analyse(const std::string &source)
{
    Analysis analysis;
    analysis.model = buildProgramModel(assembleSource(source));
    analysis.flow = computeControlFlow(analysis.model);
    analysis.constants = propagateConstants(analysis.model, analysis.flow);
    return analysis;
}
} // namespace

TEST(OptimizerConstantPropagation, ComputesArithmeticThroughABlock)
{
    const Analysis a = analyse(R"(
        lui  x5, 2
        addi x6, x5, 4
        slli x7, x6, 1
        sub  x8, x7, x5
        andi x9, x8, 0xFF
    )");
    const ConstantRegisters &exit = a.constants.at(0).atExit;
    EXPECT_EQ(exit[5], 8192u);
    EXPECT_EQ(exit[6], 8196u);
    EXPECT_EQ(exit[7], 16392u);
    EXPECT_EQ(exit[8], 8200u);
    EXPECT_EQ(exit[9], 8200u & 0xFF);
}

TEST(OptimizerConstantPropagation, NothingIsKnownAtTheStartOfTheProgramExceptZero)
{
    const Analysis a = analyse("addi x5, x6, 1\n");
    EXPECT_EQ(a.constants.at(0).atEntry[0], 0u);
    EXPECT_FALSE(a.constants.at(0).atEntry[6].has_value());
    EXPECT_FALSE(a.constants.at(0).atExit[5].has_value()); // x6 is unknown, so x5 is too
}

TEST(OptimizerConstantPropagation, LoadsAndMultipliesAreUnknown)
{
    const Analysis a = analyse("addi x5, x0, 3\n lw x6, 0(x5)\n mul x7, x5, x5\n");
    EXPECT_FALSE(a.constants.at(0).atExit[6].has_value());
    EXPECT_FALSE(a.constants.at(0).atExit[7].has_value());
}

TEST(OptimizerConstantPropagation, AgreeingPathsKeepAConstantAndDisagreeingOnesLoseIt)
{
    const Analysis a = analyse(R"(
        addi x5, x0, 1
        addi x6, x0, 2
        beq  x9, x0, join
        addi x6, x0, 3
    join:
        addi x0, x0, 0
    )");
    const BlockConstants &join = a.constants.back();
    ASSERT_TRUE(join.reachable);
    EXPECT_EQ(join.atEntry[5], 1u);                  // the same on both paths
    EXPECT_FALSE(join.atEntry[6].has_value());       // 2 on one path, 3 on the other
}

TEST(OptimizerConstantPropagation, ALoopCounterIsUnknownAtTheLoopHeader)
{
    const Analysis a = analyse(R"(
        addi x5, x0, 0
        addi x6, x0, 8
    loop:
        addi x5, x5, 1
        blt  x5, x6, loop
    )");
    const BlockConstants &loop = a.constants.at(1);
    EXPECT_FALSE(loop.atEntry[5].has_value()); // 0 from outside, 1, 2, ... around the back edge
    EXPECT_EQ(loop.atEntry[6], 8u);            // the bound never changes
}

TEST(OptimizerControlFlow, ReturnPointsAreEnteredFromAnUnknownPlace)
{
    const Analysis a = analyse(R"(
        jal  x1, callee
    after:
        addi x5, x0, 1
        jal  x0, end
    callee:
        jalr x0, 0(x1)
    end:
        addi x0, x0, 0
    )");
    EXPECT_TRUE(a.flow.hasUnknownPredecessors.at(1));  // where the callee returns to
    EXPECT_TRUE(a.flow.hasUnknownSuccessors.at(2));    // the return itself
    EXPECT_FALSE(a.flow.hasUnknownPredecessors.at(3)); // only reached by the jump
}

TEST(OptimizerControlFlow, LoadingADataAddressDoesNotMakeEveryBlockAJumpTarget)
{
    const Analysis a = analyse(R"(
    .data
    value: .word 5
    .text
        la   x5, value
    loop:
        addi x6, x6, 1
        blt  x6, x7, loop
    )");
    for (size_t b = 0; b < a.model.blocks.size(); ++b)
    {
        EXPECT_FALSE(a.flow.hasUnknownPredecessors[b]) << b;
    }
}

TEST(OptimizerControlFlow, PredecessorsMirrorSuccessors)
{
    const Analysis a = analyse(R"(
        beq  x1, x0, taken
        addi x5, x0, 1
    taken:
        addi x6, x0, 1
    )");
    ASSERT_EQ(a.model.blocks.size(), 3u);
    EXPECT_EQ(a.flow.predecessors[2], (std::vector<size_t>{0, 1}));
    EXPECT_EQ(a.flow.predecessors[1], (std::vector<size_t>{0}));
}
