// Tests for turning indexed addressing in loops into a pointer that steps through memory.

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "benchmarking/suggestion.h"
#include "optimizer/loop_unroller.h"
#include "optimizer/optimizer.h"
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

PointerResult pointersFor(const std::string &source, bool allowDeadRegisterChanges = true)
{
    PointerOptions options;
    options.allowDeadRegisterChanges = allowDeadRegisterChanges;
    return introducePointers(source, options);
}
} // namespace

TEST(OptimizerPointers, AnIndexedLoopStepsAPointerInstead)
{
    const std::string source = readFile("../benchmarks/vector_add.s");
    ASSERT_FALSE(source.empty());
    const PointerResult result = pointersFor(source);
    ASSERT_EQ(result.pointers.size(), 1u);
    const PointerReport &pointer = result.pointers[0];
    EXPECT_EQ(pointer.stride, 4);
    EXPECT_EQ(pointer.removed, 2); // the slli and the add
    EXPECT_EQ(pointer.changedDeadRegisters, std::vector<unsigned>({14, 15}));

    // The address is computed once, before the loop, and stepped inside it.
    const std::string p = "x" + std::to_string(pointer.pointer);
    const size_t loop = result.source.find("loop:");
    const size_t setup = result.source.find("slli " + p);
    ASSERT_NE(loop, std::string::npos);
    ASSERT_NE(setup, std::string::npos);
    EXPECT_LT(setup, loop);
    EXPECT_EQ(countMatches(result.source, R"(\bslli\b)"), 1u);
    EXPECT_EQ(countMatches(result.source, "addi " + p + ", " + p + ", 4"), 1u);
    // The store comes after the step, so it reaches c[i] from one element further on.
    EXPECT_EQ(countMatches(result.source, R"(sw\s+x20,\s*252\()" + p + R"(\))"), 1u);
}

TEST(OptimizerPointers, StrictModeNeedsTheAddressRegistersFreeToChange)
{
    // Without unrolling the address lands in x14 and x15, whose final values strict mode keeps.
    const PointerResult result = pointersFor(readFile("../benchmarks/vector_add.s"), false);
    EXPECT_TRUE(result.pointers.empty());
    EXPECT_FALSE(result.changed);
}

TEST(OptimizerPointers, ALoopReachedByAJumpIsLeftAlone)
{
    // The loop can also be entered by the jump at the top, which would skip a set-up placed in
    // front of it.
    const std::string source = R"(.text
    lui  x10, 0x10000
    addi x11, x0, 8
    addi x12, x0, 0
    beq  x0, x0, loop
    addi x5, x0, 1
loop:
    slli x14, x12, 2
    add  x15, x10, x14
    lw   x16, 0(x15)
    addi x16, x16, 1
    sw   x16, 0(x15)
    addi x12, x12, 1
    blt  x12, x11, loop
)";
    EXPECT_TRUE(pointersFor(source).pointers.empty());
}

TEST(OptimizerPointers, WorksAfterUnrollingByFour)
{
    // After unrolling x4 a store's offset (268) has the counter's number in the bits where other
    // instructions keep their destination; that must not look like a write to the counter.
    OptimizeOptions options;
    options.unrollFactor = 4;
    options.allowDeadRegisterChanges = true;
    const OptimizeResult result = optimizeSource(readFile("../benchmarks/vector_add.s"), options);
    EXPECT_EQ(result.pointers.size(), 1u);
}

class PointerSuggestionTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        setupVmStateDirectory();
    }
};

TEST_F(PointerSuggestionTest, PointersMakeVectorAddFasterAndStayCorrect)
{
    SuggestionRequest request;
    request.source = readFile("../benchmarks/vector_add.s");
    for (ProcessorType processor : {ProcessorType::RV5Stage_H_F, ProcessorType::RV5Stage_NH_NF})
    {
        SCOPED_TRACE(describe(processor));
        request.processor = processor;

        request.allowDeadRegisterChanges = false;
        const Suggestion strict = makeSuggestion(request);
        request.allowDeadRegisterChanges = true;
        const Suggestion withPointer = makeSuggestion(request);

        ASSERT_EQ(withPointer.result.pointers.size(), 1u);
        EXPECT_TRUE(strict.suggestionCorrect);
        EXPECT_TRUE(withPointer.suggestionCorrect);
        EXPECT_LT(withPointer.suggestionCycles, strict.suggestionCycles);
    }
}
