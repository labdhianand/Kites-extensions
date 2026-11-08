// Tests for what the CLI's --suggest and the GUI's Optimizer tab share: the complete suggestion
// (pad, optimize, measure) and the per-line changes used to tint the side-by-side view.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "benchmarking/suggestion.h"
#include "optimizer/source_text.h"
#include "utils/utils.h"

using namespace Kites;
using namespace Kites::benchmarking;
using namespace Kites::optimizer;

// ---------------------------------------------------------------------------
// Changed lines
// ---------------------------------------------------------------------------
TEST(OptimizerChangedLines, IdenticalTextsHaveNoChanges)
{
    const LineChanges changes = changedLines("a\nb\nc\n", "a\nb\nc\n");
    EXPECT_TRUE(changes.removedLines.empty());
    EXPECT_TRUE(changes.addedLines.empty());
}

TEST(OptimizerChangedLines, ASwapMarksTheMovedLineOnBothSides)
{
    const LineChanges changes =
        changedLines("one\ntwo\nthree\nfour\nfive\n", "one\ntwo\nfour\nthree\nfive\n");
    EXPECT_EQ(changes.removedLines, std::vector<int>({3}));
    EXPECT_EQ(changes.addedLines, std::vector<int>({4}));
}

TEST(OptimizerChangedLines, InsertionsAreOnlyAdded)
{
    const LineChanges changes = changedLines("a\nb\n", "a\nx\ny\nb\n");
    EXPECT_TRUE(changes.removedLines.empty());
    EXPECT_EQ(changes.addedLines, std::vector<int>({2, 3}));
}

TEST(OptimizerChangedLines, DeletionsAreOnlyRemoved)
{
    const LineChanges changes = changedLines("a\nnop\nb\nnop\n", "a\nb\n");
    EXPECT_EQ(changes.removedLines, std::vector<int>({2, 4}));
    EXPECT_TRUE(changes.addedLines.empty());
}

// ---------------------------------------------------------------------------
// Suggestions
// ---------------------------------------------------------------------------
namespace
{
// Two load-use pairs with independent work after them that can fill the load delays.
constexpr const char *kTwoLoads = R"(
.data
a: .word 3
b: .word 4
out: .word 0
.text
    lui  x10, 0x10000
    lw   x5, 0(x10)
    add  x6, x5, x5
    lw   x7, 4(x10)
    add  x8, x7, x7
    addi x11, x0, 1
    addi x12, x0, 2
    add  x9, x6, x8
    sw   x9, 8(x10)
)";
} // namespace

class SuggestionTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        setupVmStateDirectory();
    }
};

TEST_F(SuggestionTest, OnlyThePipelinesGetSuggestions)
{
    EXPECT_TRUE(canSuggestFor(ProcessorType::RV5Stage_H_F));
    EXPECT_TRUE(canSuggestFor(ProcessorType::RV5Stage_NH_NF));
    EXPECT_FALSE(canSuggestFor(ProcessorType::RVSS));

    SuggestionRequest request;
    request.source = kTwoLoads;
    request.processor = ProcessorType::RVSS;
    EXPECT_THROW((void)makeSuggestion(request), std::runtime_error);
}

TEST_F(SuggestionTest, FillsLoadDelaysWithHazardDetection)
{
    SuggestionRequest request;
    request.source = kTwoLoads;
    request.processor = ProcessorType::RV5Stage_H_F;
    const Suggestion suggestion = makeSuggestion(request);

    EXPECT_EQ(suggestion.paddingAdded, 0);
    EXPECT_EQ(suggestion.baseline, request.source);
    EXPECT_TRUE(suggestion.result.changed);
    ASSERT_TRUE(suggestion.measured);
    EXPECT_TRUE(suggestion.baselineCorrect);
    EXPECT_TRUE(suggestion.suggestionCorrect);
    EXPECT_LT(suggestion.suggestionCycles, suggestion.baselineCycles);
}

TEST_F(SuggestionTest, PadsFirstWithoutHazardDetection)
{
    SuggestionRequest request;
    request.source = kTwoLoads;
    request.processor = ProcessorType::RV5Stage_NH_NF;
    const Suggestion suggestion = makeSuggestion(request);

    // The unpadded program is wrong on this pipeline, so the comparison is against a padded one.
    EXPECT_GT(suggestion.paddingAdded, 0);
    ASSERT_TRUE(suggestion.measured);
    EXPECT_TRUE(suggestion.baselineCorrect);
    EXPECT_TRUE(suggestion.suggestionCorrect);
    EXPECT_LE(suggestion.suggestionCycles, suggestion.baselineCycles);
}

TEST_F(SuggestionTest, PlansForAnL2HitWithTheColdCacheByDefault)
{
    SuggestionRequest request;
    request.source = kTwoLoads;
    request.measure = false;

    request.memory = MemorySetup::Ideal;
    EXPECT_EQ(makeSuggestion(request).plannedLatency, 0);

    request.memory = MemorySetup::ColdCache;
    EXPECT_EQ(makeSuggestion(request).plannedLatency, kColdCacheL2Latency);

    request.plannedLatency = 9;
    EXPECT_EQ(makeSuggestion(request).plannedLatency, 9);
}

TEST_F(SuggestionTest, CanSkipMeasuring)
{
    SuggestionRequest request;
    request.source = kTwoLoads;
    request.measure = false;
    const Suggestion suggestion = makeSuggestion(request);
    EXPECT_FALSE(suggestion.measured);
    EXPECT_EQ(suggestion.baselineCycles, 0u);
    EXPECT_EQ(suggestion.suggestionCycles, 0u);
    EXPECT_FALSE(suggestion.result.source.empty());
}
