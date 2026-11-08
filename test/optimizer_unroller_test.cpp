#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "optimizer/loop_unroller.h"
#include "optimizer/optimizer.h"
#include "optimizer_test_support.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

using namespace Kites;
using namespace Kites::optimizer;
using namespace optimizer_test;

namespace
{
UnrollResult unroll(const std::string &source, int maxFactor = 4, bool fold = true)
{
    UnrollOptions options;
    options.maxFactor = maxFactor;
    options.foldAddresses = fold;
    return unrollLoops(source, options);
}

size_t countMatches(const std::string &text, const std::string &pattern)
{
    const std::regex re(pattern);
    return static_cast<size_t>(
        std::distance(std::sregex_iterator(text.begin(), text.end(), re), std::sregex_iterator()));
}

// c[i] = a[i] + b[i] for 8 elements, preceded by a loop that fills a and b.
constexpr const char *kVectorAdd = R"(
    lui  x10, 2
    addi x11, x0, 8
    addi x12, x0, 0
init:
    slli x14, x12, 2
    add  x15, x10, x14
    addi x16, x12, 1
    sw   x16, 0(x15)
    addi x17, x12, 100
    sw   x17, 32(x15)
    addi x12, x12, 1
    blt  x12, x11, init
    addi x12, x0, 0
vecadd:
    slli x14, x12, 2
    add  x15, x10, x14
    lw   x18, 0(x15)
    lw   x19, 32(x15)
    add  x20, x18, x19
    sw   x20, 64(x15)
    addi x12, x12, 1
    blt  x12, x11, vecadd
)";

// Equal registers and memory, except registers the unroller started using.
bool sameExceptRenamed(const Snapshot &a, const Snapshot &b, const std::vector<LoopReport> &loops)
{
    Snapshot left = a;
    Snapshot right = b;
    for (const LoopReport &loop : loops)
    {
        for (unsigned reg : loop.renamedTo)
        {
            left.gprs[reg] = right.gprs[reg] = 0;
        }
        for (unsigned reg : loop.changedDeadRegisters)
        {
            left.gprs[reg] = right.gprs[reg] = 0;
        }
    }
    return left == right;
}
} // namespace

// ---------------------------------------------------------------------------
// Which loops are unrolled
// ---------------------------------------------------------------------------
TEST(OptimizerUnroller, UnrollsBothCountedLoopsByTheLargestFactorThatDividesTheTripCount)
{
    const UnrollResult result = unroll(kVectorAdd);
    ASSERT_EQ(result.loops.size(), 2u);
    for (const LoopReport &loop : result.loops)
    {
        EXPECT_TRUE(loop.unrolled) << loop.reason;
        EXPECT_EQ(loop.tripCount, 8);
        EXPECT_EQ(loop.factor, 4);
    }
    EXPECT_TRUE(result.changed);
    // Each loop keeps one branch; three copies of each body were added.
    EXPECT_EQ(countMatches(result.source, R"(\bblt\b)"), 2u);
    EXPECT_EQ(countMatches(result.source, R"(\blw\b)"), 8u);
}

TEST(OptimizerUnroller, TheFactorIsCappedAndMustDivideTheTripCount)
{
    const std::string sixIterations = R"(
        addi x5, x0, 0
        addi x6, x0, 6
    loop:
        addi x7, x7, 3
        addi x5, x5, 1
        blt  x5, x6, loop
    )";
    EXPECT_EQ(unroll(sixIterations, 4).loops.at(0).factor, 3);
    EXPECT_EQ(unroll(sixIterations, 2).loops.at(0).factor, 2);

    const std::string sevenIterations = R"(
        addi x5, x0, 0
        addi x6, x0, 7
    loop:
        addi x7, x7, 3
        addi x5, x5, 1
        blt  x5, x6, loop
    )";
    const UnrollResult seven = unroll(sevenIterations, 4);
    EXPECT_FALSE(seven.loops.at(0).unrolled);
    EXPECT_EQ(seven.loops.at(0).tripCount, 7);
    EXPECT_NE(seven.loops.at(0).reason.find("divides"), std::string::npos);
    EXPECT_EQ(seven.source, sevenIterations);
}

TEST(OptimizerUnroller, CountsDownAndCompareWithZero)
{
    const UnrollResult result = unroll(R"(
        addi x5, x0, 8
    loop:
        addi x7, x7, 3
        addi x5, x5, -1
        bne  x5, x0, loop
    )");
    ASSERT_EQ(result.loops.size(), 1u);
    EXPECT_TRUE(result.loops[0].unrolled) << result.loops[0].reason;
    EXPECT_EQ(result.loops[0].tripCount, 8);
}

TEST(OptimizerUnroller, LeavesALoopWithAnUnknownBoundAlone)
{
    const UnrollResult result = unroll(R"(
        lui  x10, 2
        lw   x6, 0(x10)
        addi x5, x0, 0
    loop:
        addi x7, x7, 3
        addi x5, x5, 1
        blt  x5, x6, loop
    )");
    ASSERT_EQ(result.loops.size(), 1u);
    EXPECT_FALSE(result.loops[0].unrolled);
    EXPECT_NE(result.loops[0].reason.find("not a known constant"), std::string::npos);
    EXPECT_FALSE(result.changed);
}

TEST(OptimizerUnroller, LeavesALoopWithoutACounterAlone)
{
    // Walks a list until a zero pointer: no counter, no bound.
    const UnrollResult result = unroll(R"(
        lui  x10, 2
    loop:
        lw   x10, 0(x10)
        bne  x10, x0, loop
    )");
    ASSERT_EQ(result.loops.size(), 1u);
    EXPECT_FALSE(result.loops[0].unrolled);
    EXPECT_NE(result.loops[0].reason.find("counter"), std::string::npos);
}

TEST(OptimizerUnroller, LeavesALoopWhoseStartValueDependsOnTheWayInAlone)
{
    // The loop is entered with the counter at 0 or at 4, so its trip count is not fixed.
    const UnrollResult result = unroll(R"(
        addi x6, x0, 8
        addi x5, x0, 0
        beq  x9, x0, loop
        addi x5, x0, 4
    loop:
        addi x7, x7, 3
        addi x5, x5, 1
        blt  x5, x6, loop
    )");
    ASSERT_EQ(result.loops.size(), 1u);
    EXPECT_FALSE(result.loops[0].unrolled);
}

TEST(OptimizerUnroller, LeavesALoopAfterACallAlone)
{
    // After the call returns, the counter may have been changed by the callee.
    const UnrollResult result = unroll(R"(
        addi x6, x0, 8
        addi x5, x0, 0
        jal  x1, callee
    loop:
        addi x7, x7, 3
        addi x5, x5, 1
        blt  x5, x6, loop
        jal  x0, end
    callee:
        jalr x0, 0(x1)
    end:
        addi x0, x0, 0
    )");
    ASSERT_EQ(result.loops.size(), 1u);
    EXPECT_FALSE(result.loops[0].unrolled);
    EXPECT_NE(result.loops[0].reason.find("unknown place"), std::string::npos);
}

TEST(OptimizerUnroller, TheBoundMayBeSetLongBeforeTheLoop)
{
    // x11 is set in the first block and survives the first loop untouched.
    const UnrollResult result = unroll(kVectorAdd);
    EXPECT_TRUE(result.loops.at(1).unrolled) << result.loops.at(1).reason;
}

// ---------------------------------------------------------------------------
// The text of the unrolled loop
// ---------------------------------------------------------------------------
TEST(OptimizerUnroller, CopiesAreMarkedAndRenamedAndTheLastCopyKeepsTheOriginalNames)
{
    const std::string source =
        "    addi x5, x0, 0\n"
        "    addi x6, x0, 2\n"
        "loop:\n"
        "    addi x7, x5, 10      # temp\n"
        "    add  x8, x8, x7\n"
        "    addi x5, x5, 1\n"
        "    blt  x5, x6, loop\n";
    const UnrollResult result = unroll(source, 2, false); // the plain copies, without folding
    ASSERT_TRUE(result.loops.at(0).unrolled) << result.loops.at(0).reason;
    ASSERT_EQ(result.loops[0].renamedTo.size(), 1u); // only x7 is iteration-local
    const std::string fresh = "x" + std::to_string(result.loops[0].renamedTo[0]);

    EXPECT_EQ(result.source,
              "    addi x5, x0, 0\n"
              "    addi x6, x0, 2\n"
              "loop:\n"
              "    # loop unrolled x2\n"
              "    addi " + fresh + ", x5, 10      # temp\n"
              "    add  x8, x8, " + fresh + "\n"
              "    addi x5, x5, 1\n"
              "    addi x7, x5, 10      # temp\n"
              "    add  x8, x8, x7\n"
              "    addi x5, x5, 1\n"
              "    blt  x5, x6, loop\n");
}

TEST(OptimizerUnroller, ALabelOnTheFirstLineStaysOnlyOnTheFirstCopy)
{
    const UnrollResult result = unroll(
        "    addi x5, x0, 0\n    addi x6, x0, 2\nloop: addi x8, x8, 1\n      addi x5, x5, 1\n"
        "      blt x5, x6, loop\n",
        2, false);
    ASSERT_TRUE(result.loops.at(0).unrolled) << result.loops.at(0).reason;
    EXPECT_EQ(countMatches(result.source, R"(loop:)"), 1u);
    EXPECT_EQ(countMatches(result.source, R"(addi x8, x8, 1)"), 2u);
}

TEST(OptimizerUnroller, FoldingKeepsTheLabelWhenItRemovesTheFirstInstruction)
{
    // Both copies add 1 to x8 and to the counter; folding turns each pair into one add of 2,
    // removing the line the label is on. The label must stay, on a line of its own.
    const UnrollResult result = unroll(
        "    addi x5, x0, 0\n    addi x6, x0, 2\nloop: addi x8, x8, 1\n      addi x5, x5, 1\n"
        "      blt x5, x6, loop\n",
        2);
    ASSERT_TRUE(result.loops.at(0).unrolled) << result.loops.at(0).reason;
    EXPECT_EQ(result.loops[0].foldedInstructions, 2);
    EXPECT_EQ(countMatches(result.source, R"(\nloop:\n)"), 1u) << result.source; // on its own line
    EXPECT_EQ(countMatches(result.source, R"(addi\s+x8,\s*x8,\s*2)"), 1u) << result.source;
    EXPECT_EQ(countMatches(result.source, R"(addi\s+x5,\s*x5,\s*2)"), 1u) << result.source;
}

TEST(OptimizerUnroller, AbiRegisterNamesAreRecognisedWhenRenaming)
{
    const UnrollResult result = unroll(R"(
        addi a0, zero, 0
        addi a1, zero, 2
    loop:
        addi t0, a0, 10
        add  s1, s1, t0
        addi a0, a0, 1
        blt  a0, a1, loop
    )",
                                       2, false);
    ASSERT_TRUE(result.loops.at(0).unrolled) << result.loops.at(0).reason;
    EXPECT_EQ(result.loops[0].renamedTo.size(), 1u); // t0
    EXPECT_EQ(countMatches(result.source, R"(\bt0\b)"), 2u); // only in the last copy
}

TEST(OptimizerUnroller, WithNoFreeRegistersItStillUnrollsWithoutRenaming)
{
    std::string source = "addi x5, x0, 0\naddi x6, x0, 2\n";
    for (int r = 7; r < 32; ++r)
    {
        source += "addi x" + std::to_string(r) + ", x0, 1\n"; // use every register
    }
    source += "loop:\naddi x7, x5, 10\nadd x8, x8, x7\naddi x5, x5, 1\nblt x5, x6, loop\n";
    const UnrollResult result = unroll(source, 2);
    ASSERT_TRUE(result.loops.at(0).unrolled) << result.loops.at(0).reason;
    EXPECT_TRUE(result.loops[0].renamedTo.empty());
}

TEST(OptimizerUnroller, ASourceThatDoesNotAssembleIsAnError)
{
    EXPECT_THROW((void)unroll("not assembly\n"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// On the simulator
// ---------------------------------------------------------------------------
namespace
{
// A loop with a random body: integer arithmetic on x5-x9, iteration-local temporaries x16-x19 (so
// renaming has work to do), and loads and stores through bases that move every iteration.
std::string randomLoopProgram(std::mt19937 &rng, int tripCount)
{
    auto pick = [&](int lowest, int highest)
    { return std::uniform_int_distribution<int>(lowest, highest)(rng); };
    auto data = [&]() { return "x" + std::to_string(pick(5, 9)); };
    auto temp = [&]() { return "x" + std::to_string(pick(16, 19)); };
    auto any = [&]() { return pick(0, 1) ? data() : temp(); };
    auto base = [&]() { return std::string(pick(0, 1) ? "x10" : "x11"); };

    std::ostringstream out;
    out << "li x5, 11\nli x6, 22\nli x7, 33\nli x8, 44\nli x9, 55\n";
    out << "li x10, 8192\nli x11, 8196\n";
    out << "addi x20, x0, 0\naddi x21, x0, " << tripCount << "\n";
    out << "loop:\n";

    const int bodyLength = pick(4, 10);
    const int counterPosition = pick(0, bodyLength);
    for (int i = 0; i <= bodyLength; ++i)
    {
        if (i == counterPosition)
        {
            out << "addi x20, x20, 1\n";
        }
        if (i == bodyLength)
        {
            break;
        }
        switch (pick(0, 7))
        {
        case 0:
            out << "add " << any() << ", " << any() << ", " << any() << "\n";
            break;
        case 1:
            out << "addi " << temp() << ", " << data() << ", " << pick(-8, 8) << "\n";
            break;
        case 2:
            out << "sub " << data() << ", " << any() << ", " << any() << "\n";
            break;
        case 3:
            out << "sw " << any() << ", " << pick(0, 3) * 4 << "(" << base() << ")\n";
            break;
        case 4:
            out << "lw " << any() << ", " << pick(0, 3) * 4 << "(" << base() << ")\n";
            break;
        case 5:
            out << "addi " << base() << ", " << base() << ", 4\n";
            break;
        case 6:
            out << "xor " << data() << ", " << temp() << ", " << data() << "\n";
            break;
        default:
            out << "slli " << temp() << ", " << any() << ", " << pick(0, 3) << "\n";
            break;
        }
    }
    out << (pick(0, 1) ? "blt" : "bne") << " x20, x21, loop\n";
    return out.str();
}
} // namespace

TEST(OptimizerUnrollerSimulation, UnrolledRandomLoopsComputeTheSameResult)
{
    setupVmStateDirectory();
    std::mt19937 rng(31337);
    const size_t programCount = programCountFromEnvironment("KITES_UNROLL_PROGRAMS", 40);
    size_t unrolledCount = 0;
    size_t renamedCount = 0;

    for (size_t i = 0; i < programCount; ++i)
    {
        const int trips = std::uniform_int_distribution<int>(1, 12)(rng);
        const std::string source = randomLoopProgram(rng, trips);
        SCOPED_TRACE("trip count " + std::to_string(trips) + ":\n" + source);

        const UnrollResult result = unroll(source, 4);
        ASSERT_EQ(result.loops.size(), 1u);
        EXPECT_EQ(result.loops[0].tripCount, trips);

        const Snapshot before = runOn<RVSSProcessor>(assembleSource(source)).snapshot;
        const Snapshot after = runOn<RVSSProcessor>(assembleSource(result.source)).snapshot;
        EXPECT_TRUE(sameExceptRenamed(before, after, result.loops)) << result.source;
        // Without allowDeadRegisterChanges, every original register keeps its final value.
        EXPECT_TRUE(result.loops[0].changedDeadRegisters.empty());

        unrolledCount += result.loops[0].unrolled ? 1 : 0;
        renamedCount += result.loops[0].renamedTo.empty() ? 0 : 1;
    }

    // Guards the test itself: most loops are unrolled and most of those rename something.
    EXPECT_GT(unrolledCount, programCount / 2);
    EXPECT_GT(renamedCount, programCount / 4);
}

TEST(OptimizerUnrollerSimulation, FoldedRandomLoopsComputeTheSameResult)
{
    // The same random loops, letting registers nothing reads after the loop end differently, so
    // the repeated address arithmetic can go. Checked on the single-cycle processor, and after
    // scheduling on the pipeline with hazard detection and forwarding.
    setupVmStateDirectory();
    std::mt19937 rng(4242);
    const size_t programCount = programCountFromEnvironment("KITES_UNROLL_PROGRAMS", 40);
    size_t foldedCount = 0;

    for (size_t i = 0; i < programCount; ++i)
    {
        const int trips = std::uniform_int_distribution<int>(1, 12)(rng);
        const std::string source = randomLoopProgram(rng, trips);
        SCOPED_TRACE("trip count " + std::to_string(trips) + ":\n" + source);

        UnrollOptions unrollOptions;
        unrollOptions.maxFactor = 4;
        unrollOptions.allowDeadRegisterChanges = true;
        const UnrollResult result = unrollLoops(source, unrollOptions);
        ASSERT_EQ(result.loops.size(), 1u);

        const Snapshot reference = runOn<RVSSProcessor>(assembleSource(source)).snapshot;
        const Snapshot unrolled = runOn<RVSSProcessor>(assembleSource(result.source)).snapshot;
        EXPECT_TRUE(sameExceptRenamed(reference, unrolled, result.loops)) << result.source;
        // The loop counter and bound are read by the loop itself, so they always survive.
        for (unsigned reg : result.loops[0].changedDeadRegisters)
        {
            EXPECT_TRUE(reg != 20 && reg != 21) << "x" << reg;
        }

        OptimizeOptions options;
        options.variant = PipelineVariant::H_F;
        options.unrollFactor = 4;
        options.allowDeadRegisterChanges = true;
        const OptimizeResult optimized = optimizeSource(source, options);
        const Snapshot pipelined = runOn<RV5StageProcessorHF>(assembleSource(optimized.source)).snapshot;
        EXPECT_TRUE(sameExceptRenamed(reference, pipelined, optimized.loops)) << optimized.source;

        foldedCount += result.loops[0].foldedInstructions > 0 ? 1 : 0;
    }

    // Guards the test itself: folding has something to do in a good share of the loops.
    EXPECT_GT(foldedCount, programCount / 5);
}

TEST(OptimizerUnrollerSimulation, UnrollingThenReorderingIsCorrectAndFasterOnThePipelines)
{
    setupVmStateDirectory();
    const Snapshot reference = runOn<RVSSProcessor>(assembleSource(kVectorAdd)).snapshot;

    OptimizeOptions options;
    options.unrollFactor = 4;

    options.variant = PipelineVariant::H_NF;
    const OptimizeResult noForwarding = optimizeSource(kVectorAdd, options);
    const RunResult hnfBefore = runOn<RV5StageProcessorHNF>(assembleSource(kVectorAdd));
    const RunResult hnfAfter = runOn<RV5StageProcessorHNF>(assembleSource(noForwarding.source));
    EXPECT_TRUE(sameExceptRenamed(hnfAfter.snapshot, reference, noForwarding.loops));
    EXPECT_LT(hnfAfter.cycles, hnfBefore.cycles);

    options.variant = PipelineVariant::H_F;
    const OptimizeResult forwarding = optimizeSource(kVectorAdd, options);
    const RunResult hfBefore = runOn<RV5StageProcessorHF>(assembleSource(kVectorAdd));
    const RunResult hfAfter = runOn<RV5StageProcessorHF>(assembleSource(forwarding.source));
    EXPECT_TRUE(sameExceptRenamed(hfAfter.snapshot, reference, forwarding.loops));
    EXPECT_LT(hfAfter.cycles, hfBefore.cycles);
}
