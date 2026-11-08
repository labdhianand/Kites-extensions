// Tests for removing the address arithmetic an unrolled loop repeats.

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "benchmarking/program_runner.h"
#include "benchmarking/suggestion.h"
#include "optimizer/loop_unroller.h"
#include "utils/utils.h"

using namespace Kites;
using namespace Kites::benchmarking;
using namespace Kites::optimizer;

namespace
{
std::string readFile(const std::string &path)
{
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

size_t countMatches(const std::string &text, const std::string &pattern)
{
    const std::regex re(pattern);
    return static_cast<size_t>(
        std::distance(std::sregex_iterator(text.begin(), text.end(), re), std::sregex_iterator()));
}

UnrollResult unrollBy2(const std::string &source, bool allowDeadRegisterChanges)
{
    UnrollOptions options;
    options.maxFactor = 2;
    options.allowDeadRegisterChanges = allowDeadRegisterChanges;
    return unrollLoops(source, options);
}
} // namespace

TEST(OptimizerAddressFolding, TheSecondCopyReusesTheFirstCopysAddress)
{
    const std::string source = readFile("../benchmarks/vector_add.s");
    ASSERT_FALSE(source.empty());
    const UnrollResult result = unrollBy2(source, true);
    ASSERT_EQ(result.loops.size(), 1u);
    const LoopReport &loop = result.loops[0];
    ASSERT_TRUE(loop.unrolled);

    // The second copy's slli and add go, and the two counter steps become one `addi i, i, 2`.
    EXPECT_EQ(loop.foldedInstructions, 3);
    EXPECT_EQ(countMatches(result.source, R"(\bslli\b)"), 1u);
    EXPECT_EQ(countMatches(result.source, R"(addi\s+x12,\s*x12,\s*2\b)"), 1u);
    // a[i+1] and b[i+1] are now reached from the first copy's address, 4 bytes further on.
    EXPECT_EQ(countMatches(result.source, R"(lw\s+x\d+,\s*4\(x\d+\))"), 1u);
    EXPECT_EQ(countMatches(result.source, R"(lw\s+x\d+,\s*132\(x\d+\))"), 1u);
    // x14 and x15 are not read after the loop, so they may now end differently; x12 is.
    EXPECT_EQ(loop.changedDeadRegisters, std::vector<unsigned>({14, 15}));
}

TEST(OptimizerAddressFolding, StrictModeKeepsEveryRegister)
{
    const std::string source = readFile("../benchmarks/vector_add.s");
    const UnrollResult result = unrollBy2(source, false);
    ASSERT_TRUE(result.loops.at(0).unrolled);
    EXPECT_TRUE(result.loops[0].changedDeadRegisters.empty());
}

TEST(OptimizerAddressFolding, OffsetsThatDoNotFitAnImmediateAreLeftAlone)
{
    // Elements 2 KiB apart: the second copy's address is 2048 bytes on, which no load can reach
    // from the first copy's address, so the address computation has to stay.
    const std::string source = R"(.text
    lui  x10, 0x10000
    addi x11, x0, 4
    addi x12, x0, 0
loop:
    slli x14, x12, 11
    add  x15, x10, x14
    lw   x16, 0(x15)
    addi x16, x16, 1
    sw   x16, 0(x15)
    addi x12, x12, 1
    blt  x12, x11, loop
)";
    const UnrollResult result = unrollBy2(source, true);
    ASSERT_TRUE(result.loops.at(0).unrolled);
    EXPECT_EQ(countMatches(result.source, R"(\bslli\b)"), 2u);
    EXPECT_EQ(countMatches(result.source, R"(2048\()"), 0u);
}

class AddressFoldingSuggestionTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        setupVmStateDirectory();
    }
};

TEST_F(AddressFoldingSuggestionTest, FoldingMakesVectorAddFasterAndStaysCorrect)
{
    SuggestionRequest request;
    request.source = readFile("../benchmarks/vector_add.s");
    request.unrollFactor = 2;
    for (ProcessorType processor : {ProcessorType::RV5Stage_H_F, ProcessorType::RV5Stage_NH_NF})
    {
        SCOPED_TRACE(describe(processor));
        request.processor = processor;

        request.allowDeadRegisterChanges = false;
        const Suggestion strict = makeSuggestion(request);
        request.allowDeadRegisterChanges = true;
        const Suggestion folded = makeSuggestion(request);

        EXPECT_TRUE(strict.suggestionCorrect);
        EXPECT_TRUE(folded.suggestionCorrect);
        EXPECT_LT(folded.suggestionCycles, strict.suggestionCycles);
        EXPECT_EQ(folded.changedDeadRegisters, std::vector<unsigned>({14, 15}));
    }
}
