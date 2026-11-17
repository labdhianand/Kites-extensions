// The optimizer's runner and verification with programs that read and print.

#include <gtest/gtest.h>

#include "benchmarking/program_runner.h"
#include "benchmarking/suggestion.h"
#include "utils/utils.h"

using namespace Kites;
using namespace Kites::benchmarking;

namespace
{
// Reads two numbers and prints their sum.
constexpr const char *kAdd = R"(
    li   a7, 5
    ecall
    mv   s0, a0
    li   a7, 5
    ecall
    add  a0, a0, s0
    li   a7, 1
    ecall
)";

const ProcessorType kAllModels[] = {ProcessorType::RVSS, ProcessorType::RV5Stage_NH_NF,
                                    ProcessorType::RV5Stage_H_NF, ProcessorType::RV5Stage_NH_F,
                                    ProcessorType::RV5Stage_H_F};
} // namespace

TEST(OptimizerIo, RunnerFeedsInputAndReturnsOutput)
{
    setupVmStateDirectory();
    for (ProcessorType model : {ProcessorType::RVSS, ProcessorType::RV5Stage_H_F})
    {
        const ProgramRun run = runProgram(kAdd, model, MemorySetup::Ideal, "3 4");
        EXPECT_TRUE(run.finished());
        EXPECT_EQ(run.output, "7");
    }
}

TEST(OptimizerIo, RunnerReportsAProgramThatNeedsInputInsteadOfWaiting)
{
    setupVmStateDirectory();
    for (ProcessorType model : kAllModels)
    {
        const ProgramRun run = runProgram(kAdd, model);
        EXPECT_EQ(run.end, RunEnd::NeedsInput);
        EXPECT_FALSE(run.finished());
    }
    // One number is not enough for two reads.
    EXPECT_EQ(runProgram(kAdd, ProcessorType::RV5Stage_H_F, MemorySetup::Ideal, "3").end,
              RunEnd::NeedsInput);
}

TEST(OptimizerIo, RunnerStopsAProgramThatNeverEnds)
{
    setupVmStateDirectory();
    for (ProcessorType model : {ProcessorType::RVSS, ProcessorType::RV5Stage_H_F})
    {
        const ProgramRun run =
            runProgram("loop:\n j loop\n", model, MemorySetup::Ideal, "", 2000);
        EXPECT_EQ(run.end, RunEnd::CycleLimit);
    }
}

TEST(OptimizerIo, PollingForAKeyThatIsNeverPressedIsCaught)
{
    setupVmStateDirectory();
    const std::string source = R"(
        lui  t0, 0x20000
    wait:
        lw   t1, 0(t0)
        andi t1, t1, 1
        beq  t1, x0, wait
    )";
    EXPECT_EQ(runProgram(source, ProcessorType::RV5Stage_H_F, MemorySetup::Ideal, "", 3000).end,
              RunEnd::CycleLimit);
    EXPECT_TRUE(runProgram(source, ProcessorType::RV5Stage_H_F, MemorySetup::Ideal, "x", 3000)
                    .finished());
}

TEST(OptimizerIo, SuggestionIsVerifiedWithTheSameInputAndOutput)
{
    setupVmStateDirectory();
    SuggestionRequest request;
    request.source = kAdd;
    request.input = "3 4";
    for (ProcessorType model : {ProcessorType::RV5Stage_H_F, ProcessorType::RV5Stage_NH_NF})
    {
        request.processor = model;
        const Suggestion suggestion = makeSuggestion(request);
        EXPECT_TRUE(suggestion.measured) << suggestion.measureProblem;
        EXPECT_EQ(suggestion.output, "7");
        EXPECT_TRUE(suggestion.baselineCorrect);
        EXPECT_TRUE(suggestion.suggestionCorrect);
    }
}

TEST(OptimizerIo, WithoutTheInputItNeedsTheSuggestionIsNotMeasured)
{
    setupVmStateDirectory();
    SuggestionRequest request;
    request.source = kAdd;
    const Suggestion suggestion = makeSuggestion(request);
    EXPECT_FALSE(suggestion.measured);
    EXPECT_FALSE(suggestion.measureProblem.empty());
    EXPECT_FALSE(suggestion.result.source.empty()); // the suggestion itself is still produced
}

// The check compares what is printed too: a program that changes the output is caught.
TEST(OptimizerIo, ADifferentOutputIsNotIdentical)
{
    setupVmStateDirectory();
    const ProgramRun a = runProgram(kAdd, ProcessorType::RVSS, MemorySetup::Ideal, "3 4");
    const ProgramRun b = runProgram(kAdd, ProcessorType::RVSS, MemorySetup::Ideal, "3 5");
    EXPECT_TRUE(sameResult(a.state, a.state));
    EXPECT_NE(a.output, b.output);
}

// The ecall reads a7 and a0 when it retires, so a pipeline that cannot detect hazards needs no
// padding for them.
TEST(OptimizerIo, NoPaddingIsNeededBeforeAnEcall)
{
    setupVmStateDirectory();
    SuggestionRequest request;
    request.source = "li a0, 5\nli a7, 1\necall\n";
    for (ProcessorType model : {ProcessorType::RV5Stage_NH_NF, ProcessorType::RV5Stage_NH_F})
    {
        request.processor = model;
        const Suggestion suggestion = makeSuggestion(request);
        EXPECT_EQ(suggestion.paddingAdded, 0);
        EXPECT_TRUE(suggestion.measured) << suggestion.measureProblem;
        EXPECT_EQ(suggestion.output, "5");
        EXPECT_TRUE(suggestion.baselineCorrect);
    }
}
