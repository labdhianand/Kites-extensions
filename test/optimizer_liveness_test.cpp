#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "optimizer/liveness.h"
#include "optimizer_test_support.h"

using namespace Kites;
using namespace Kites::optimizer;
using namespace optimizer_test;

namespace
{
std::vector<BlockLiveness> livenessOf(const std::string &source)
{
    return computeLiveness(buildProgramModel(assembleSource(source)));
}

RegisterSet gprs(std::initializer_list<int> indices)
{
    RegisterSet set;
    for (int index : indices)
    {
        set.set(static_cast<size_t>(index));
    }
    return set;
}
} // namespace

TEST(OptimizerLiveness, ValueReadAfterTheBlockIsLiveOut)
{
    const auto live = livenessOf(R"(
        addi x2, x0, 5
        addi x3, x0, 6
    next:
        add  x4, x2, x2
    )");
    ASSERT_EQ(live.size(), 2u);
    EXPECT_EQ(live[0].liveOut, gprs({2}));
    EXPECT_TRUE(live[1].liveOut.none()); // the program ends after the last block
}

TEST(OptimizerLiveness, LiveInIsWhatTheBlockReadsBeforeWritingPlusWhatPassesThrough)
{
    const auto live = livenessOf(R"(
        addi x5, x0, 1
        add  x6, x7, x7
    next:
        add  x8, x9, x5
    )");
    ASSERT_EQ(live.size(), 2u);
    // Block 1 reads x9 and x5. Block 0 writes x5, so only x9 passes through; it also reads x7.
    EXPECT_EQ(live[1].liveIn, gprs({5, 9}));
    EXPECT_EQ(live[0].liveIn, gprs({7, 9}));
    EXPECT_EQ(live[0].liveOut, gprs({5, 9}));
}

TEST(OptimizerLiveness, ARegisterOverwrittenBeforeBeingReadIsNotLive)
{
    const auto live = livenessOf(R"(
        addi x2, x0, 5
    next:
        addi x2, x0, 9
        add  x3, x2, x2
    )");
    ASSERT_EQ(live.size(), 2u);
    EXPECT_TRUE(live[0].liveOut.none());
}

TEST(OptimizerLiveness, BothSidesOfABranchAreSuccessors)
{
    const auto live = livenessOf(R"(
        beq  x1, x0, taken
        addi x5, x0, 1
        add  x6, x2, x2
    taken:
        add  x7, x3, x3
    )");
    ASSERT_EQ(live.size(), 3u);
    // The fall-through block reads x2 and the branch target reads x3.
    EXPECT_TRUE(live[0].liveOut[2]);
    EXPECT_TRUE(live[0].liveOut[3]);
}

TEST(OptimizerLiveness, LoopCarriedRegistersAreLiveAroundTheBackEdge)
{
    const auto live = livenessOf(R"(
        addi x1, x0, 4
    loop:
        add  x2, x2, x1
        addi x1, x1, -1
        bne  x1, x0, loop
    after:
        add  x3, x2, x2
    )");
    ASSERT_EQ(live.size(), 3u);
    // Inside the loop both the counter and the running sum are needed on the next iteration.
    EXPECT_TRUE(live[1].liveOut[1]);
    EXPECT_TRUE(live[1].liveOut[2]);
    EXPECT_TRUE(live[1].liveIn[1]);
    EXPECT_TRUE(live[1].liveIn[2]);
}

TEST(OptimizerLiveness, AJumpThroughARegisterMakesEverythingLive)
{
    const auto live = livenessOf(R"(
        addi x5, x0, 1
        jalr x0, 0(x1)
    after:
        addi x6, x0, 1
    )");
    ASSERT_EQ(live.size(), 2u);
    EXPECT_TRUE(live[0].liveOut.all());
}

TEST(OptimizerLiveness, APlainJumpDoesNotFallThrough)
{
    const auto live = livenessOf(R"(
        jal  x0, far
    skipped:
        add  x4, x2, x2
    far:
        add  x5, x3, x3
    )");
    ASSERT_EQ(live.size(), 3u);
    EXPECT_TRUE(live[0].liveOut[3]);  // the jump target
    EXPECT_FALSE(live[0].liveOut[2]); // the skipped block is never reached from here
}

TEST(OptimizerLiveness, ACallAlsoContinuesAtTheNextBlockAfterTheCalleeReturns)
{
    const auto live = livenessOf(R"(
        jal  x1, callee
    after:
        add  x4, x2, x2
    callee:
        add  x5, x3, x3
    )");
    ASSERT_EQ(live.size(), 3u);
    EXPECT_TRUE(live[0].liveOut[3]); // the callee
    EXPECT_TRUE(live[0].liveOut[2]); // what the code after the call reads
}

TEST(OptimizerLiveness, EcallArgumentsAreReadByTheEcallItself)
{
    const auto live = livenessOf(R"(
        addi a7, x0, 10
        ecall
    )");
    ASSERT_EQ(live.size(), 1u);
    EXPECT_TRUE(live[0].liveOut.none()); // a7 is written inside the block before the ecall reads it
    EXPECT_TRUE(live[0].liveIn[10]);     // a0 is read by the ecall without being written first
}

TEST(OptimizerLiveness, EmptyProgramHasNoBlocks)
{
    EXPECT_TRUE(computeLiveness(ProgramModel{}).empty());
}
