// Tests for the pieces the optimizer's command-line tools are built from: the program runner and
// the unified diff.

#include <gtest/gtest.h>

#include <string>

#include "benchmarking/program_runner.h"
#include "optimizer/source_text.h"
#include "utils/utils.h"

using namespace Kites;
using namespace Kites::benchmarking;
using namespace Kites::optimizer;

// ---------------------------------------------------------------------------
// Program runner
// ---------------------------------------------------------------------------
namespace
{
// Loads a word from the data section and stores twice its value next to it.
constexpr const char *kDoubler = R"(
.data
value: .word 21
twice: .word 0
.text
    lui  x10, 0x10000
    addi x0, x0, 0
    addi x0, x0, 0
    addi x0, x0, 0
    lw   x5, 0(x10)
    addi x0, x0, 0
    addi x0, x0, 0
    addi x0, x0, 0
    add  x6, x5, x5
    addi x0, x0, 0
    addi x0, x0, 0
    addi x0, x0, 0
    sw   x6, 4(x10)
)";
} // namespace

class ProgramRunnerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        setupVmStateDirectory();
    }
};

TEST_F(ProgramRunnerTest, CapturesRegistersAndTheDataSection)
{
    const ProgramRun run = runProgram(kDoubler, ProcessorType::RV5Stage_H_F);
    EXPECT_EQ(run.state.gprs[5], 21u);
    EXPECT_EQ(run.state.gprs[6], 42u);
    ASSERT_GE(run.state.data.size(), 2u);
    EXPECT_EQ(run.state.data[0], 21u);
    EXPECT_EQ(run.state.data[1], 42u);
    EXPECT_GT(run.cycles, 0u);
}

TEST_F(ProgramRunnerTest, EveryProcessorModelComputesTheSameResult)
{
    const ProgramState reference = runProgram(kDoubler, ProcessorType::RVSS).state;
    for (ProcessorType type : {ProcessorType::RV5Stage_NH_NF, ProcessorType::RV5Stage_H_NF,
                               ProcessorType::RV5Stage_NH_F, ProcessorType::RV5Stage_H_F})
    {
        EXPECT_TRUE(sameResult(reference, runProgram(kDoubler, type).state))
            << static_cast<int>(type);
    }
}

TEST_F(ProgramRunnerTest, AColdCacheCostsCyclesButNotCorrectness)
{
    const ProgramRun ideal = runProgram(kDoubler, ProcessorType::RV5Stage_H_F);
    const ProgramRun cold = runProgram(kDoubler, ProcessorType::RV5Stage_H_F, MemorySetup::ColdCache);
    EXPECT_GT(cold.cycles, ideal.cycles);
    EXPECT_TRUE(sameResult(ideal.state, cold.state));
}

TEST_F(ProgramRunnerTest, SameResultCanIgnoreRegisters)
{
    ProgramState a;
    ProgramState b;
    b.gprs[20] = 99;
    EXPECT_FALSE(sameResult(a, b));
    EXPECT_TRUE(sameResult(a, b, {20}));
    b.data.push_back(1);
    EXPECT_FALSE(sameResult(a, b, {20}));
}

TEST_F(ProgramRunnerTest, ASourceThatDoesNotAssembleIsAnError)
{
    EXPECT_THROW((void)runProgram("not assembly\n", ProcessorType::RVSS), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Unified diff
// ---------------------------------------------------------------------------
TEST(OptimizerUnifiedDiff, IdenticalTextsGiveNoDiff)
{
    EXPECT_EQ(unifiedDiff("a\nb\n", "a\nb\n", "x", "y"), "");
}

TEST(OptimizerUnifiedDiff, ASwapIsShownWithContext)
{
    const std::string before = "one\ntwo\nthree\nfour\nfive\nsix\n";
    const std::string after = "one\ntwo\nfour\nthree\nfive\nsix\n";
    EXPECT_EQ(unifiedDiff(before, after, "old.s", "new.s", 1),
              "--- old.s\n"
              "+++ new.s\n"
              "@@ -2,4 +2,4 @@\n"
              " two\n"
              "-three\n"
              " four\n"
              "+three\n"
              " five\n");
}

TEST(OptimizerUnifiedDiff, FarApartChangesGetSeparateHunks)
{
    const std::string before = "a\nb\nc\nd\ne\nf\ng\nh\n";
    const std::string after = "A\nb\nc\nd\ne\nf\ng\nH\n";
    const std::string diff = unifiedDiff(before, after, "x", "y", 1);
    EXPECT_NE(diff.find("@@ -1,2 +1,2 @@\n-a\n+A\n b\n"), std::string::npos);
    EXPECT_NE(diff.find("@@ -7,2 +7,2 @@\n g\n-h\n+H\n"), std::string::npos);
}

TEST(OptimizerUnifiedDiff, InsertedLinesAreMarked)
{
    const std::string diff = unifiedDiff("a\nb\n", "a\nx\ny\nb\n", "x", "y", 1);
    EXPECT_NE(diff.find(" a\n+x\n+y\n b\n"), std::string::npos);
}
