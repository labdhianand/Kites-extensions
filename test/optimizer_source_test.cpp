#include <gtest/gtest.h>

#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include "optimizer/optimizer.h"
#include "optimizer_test_support.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rv5s/rv5s_processor_nh_f.h"
#include "processor/rv5s/rv5s_processor_nh_nf.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

using namespace Kites;
using namespace Kites::optimizer;
using namespace optimizer_test;

namespace
{
OptimizeResult optimize(const std::string &source, PipelineVariant variant, bool dropNops = false)
{
    OptimizeOptions options;
    options.variant = variant;
    options.dropNops = dropNops;
    return optimizeSource(source, options);
}
} // namespace

// ---------------------------------------------------------------------------
// The text of the suggestion
// ---------------------------------------------------------------------------
TEST(OptimizerSource, MovesInstructionsBetweenTheirLinesAndKeepsComments)
{
    const std::string source =
        "    lw  x1, 0(x10)     # first\n"
        "    add x2, x1, x1     # use first\n"
        "    lw  x3, 4(x10)     # second\n"
        "    add x4, x3, x3     # use second\n";
    const OptimizeResult result = optimize(source, PipelineVariant::H_F);

    EXPECT_TRUE(result.changed);
    EXPECT_EQ(result.source,
              "    lw  x1, 0(x10)     # first\n"
              "    lw  x3, 4(x10)     # second\n"
              "    add x2, x1, x1     # use first\n"
              "    add x4, x3, x3     # use second\n");
}

TEST(OptimizerSource, ALabelStaysAtTheStartOfItsBlockEvenWhenTheFirstInstructionChanges)
{
    const std::string source =
        "start: addi x5, x0, 1\n"
        "       lw   x1, 0(x10)\n"
        "       add  x2, x1, x1\n"
        "       addi x6, x0, 2\n";
    const OptimizeResult result = optimize(source, PipelineVariant::H_F);

    EXPECT_EQ(result.source,
              "start: lw   x1, 0(x10)\n"
              "       addi x5, x0, 1\n"
              "       add  x2, x1, x1\n"
              "       addi x6, x0, 2\n");
}

TEST(OptimizerSource, CommentLinesAndBlankLinesStayWhereTheyAre)
{
    const std::string source =
        "    lw  x1, 0(x10)\n"
        "    # compute\n"
        "    add x2, x1, x1\n"
        "\n"
        "    addi x5, x0, 1\n";
    const OptimizeResult result = optimize(source, PipelineVariant::H_F);

    EXPECT_EQ(result.source,
              "    lw  x1, 0(x10)\n"
              "    # compute\n"
              "    addi x5, x0, 1\n"
              "\n"
              "    add x2, x1, x1\n");
}

TEST(OptimizerSource, WithoutHazardDetectionTheNeededPaddingIsInserted)
{
    const std::string source =
        "lw  x1, 0(x10)\n"
        "add x2, x1, x1\n"
        "lw  x3, 4(x10)\n"
        "add x4, x3, x3\n";
    const OptimizeResult result = optimize(source, PipelineVariant::NH_NF);

    EXPECT_TRUE(result.changed);
    EXPECT_EQ(result.source,
              "lw  x1, 0(x10)\n"
              "lw  x3, 4(x10)\n"
              "addi x0, x0, 0  # padding\n"
              "add x2, x1, x1\n"
              "add x4, x3, x3\n");
}

TEST(OptimizerSource, PaddingGoesAfterTheLabelOfItsSlot)
{
    // The padding lands before the first instruction of the block, which carries the label.
    const std::string source =
        "top: lw  x1, 0(x10)\n"
        "     add x2, x1, x1\n"
        "     lw  x3, 4(x10)\n"
        "     add x4, x3, x3\n";
    const OptimizeResult result = optimize(source, PipelineVariant::H_NF);
    EXPECT_TRUE(result.changed);

    // Hazard detection: no padding is inserted, the label stays on the first line.
    EXPECT_EQ(result.source.rfind("top: lw", 0), 0u);
    EXPECT_EQ(result.source.find("padding"), std::string::npos);
}

TEST(OptimizerSource, DroppedNopsDisappearButTheirLabelsDoNot)
{
    // Both NOPs of the `skip` block are dropped, but jumps still land on `skip`.
    const std::string source =
        "    beq x1, x0, skip\n"
        "    addi x5, x0, 1\n"
        "skip: addi x0, x0, 0\n"
        "      addi x0, x0, 0\n"
        "next: addi x6, x0, 1\n";
    const OptimizeResult result = optimize(source, PipelineVariant::H_F, /*dropNops=*/true);

    EXPECT_TRUE(result.changed);
    EXPECT_EQ(result.source,
              "    beq x1, x0, skip\n"
              "    addi x5, x0, 1\n"
              "skip:\n"
              "next: addi x6, x0, 1\n");
}

TEST(OptimizerSource, AnUnchangedProgramComesBackIdentical)
{
    const std::string source = "add x2, x3, x4\nadd x5, x6, x7\n";
    const OptimizeResult result = optimize(source, PipelineVariant::H_F);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.source, source);
    EXPECT_EQ(result.originalSlots, result.optimizedSlots);
}

TEST(OptimizerSource, WindowsLineEndingsAndMissingFinalNewlineArePreserved)
{
    const std::string source = "lw x1, 0(x10)\r\nadd x2, x1, x1\r\naddi x5, x0, 1";
    const OptimizeResult result = optimize(source, PipelineVariant::H_F);
    EXPECT_EQ(result.source, "lw x1, 0(x10)\r\naddi x5, x0, 1\r\nadd x2, x1, x1");
}

TEST(OptimizerSource, ReportsPerBlockAndTotalSavings)
{
    const std::string source =
        "lw  x1, 0(x10)\n"
        "add x2, x1, x1\n"
        "lw  x3, 4(x10)\n"
        "add x4, x3, x3\n";
    const OptimizeResult result = optimize(source, PipelineVariant::H_F);

    ASSERT_EQ(result.blocks.size(), 1u);
    EXPECT_TRUE(result.blocks[0].changed);
    EXPECT_EQ(result.blocks[0].firstLine, 1u);
    EXPECT_EQ(result.blocks[0].lastLine, 4u);
    EXPECT_EQ(result.blocks[0].originalSlots, 6);
    EXPECT_EQ(result.blocks[0].optimizedSlots, 4);
    EXPECT_EQ(result.originalSlots, 6);
    EXPECT_EQ(result.optimizedSlots, 4);
}

TEST(OptimizerSource, ABlockLeftAloneSaysWhy)
{
    const OptimizeResult result = optimize("flw f1, 0(x10)\nfadd.s f2, f1, f1\naddi x5, x0, 1\n",
                                           PipelineVariant::H_F);
    ASSERT_EQ(result.blocks.size(), 1u);
    EXPECT_FALSE(result.blocks[0].changed);
    EXPECT_NE(result.blocks[0].reason.find("floating-point"), std::string::npos);
}

TEST(OptimizerSource, ASourceThatDoesNotAssembleIsAnError)
{
    EXPECT_THROW((void)optimize("this is not assembly\n", PipelineVariant::H_F),
                 std::runtime_error);
}

TEST(OptimizerSource, TheSuggestionAlwaysAssemblesAndKeepsTheInstructionsWhenNothingIsDropped)
{
    std::mt19937 rng(2718);
    for (int i = 0; i < 25; ++i)
    {
        const std::string source = randomProgram(rng, 16, /*includeFloatingPoint=*/false);
        SCOPED_TRACE(source);
        const OptimizeResult result = optimize(source, PipelineVariant::H_NF);
        const AssembledProgram before = assembleSource(source);
        const AssembledProgram after = assembleSource(result.source); // must not throw

        // Reordering without dropping or padding is a permutation of the same instructions.
        std::vector<uint32_t> a = before.text_buffer;
        std::vector<uint32_t> b = after.text_buffer;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        EXPECT_EQ(a, b);
    }
}

// ---------------------------------------------------------------------------
// Whole programs, with loops, on the simulator
// ---------------------------------------------------------------------------
namespace
{
// Sum of an array, two elements per iteration (loads feeding adds inside a loop).
constexpr const char *kArraySum = R"(
    lui  x10, 2
    addi x11, x0, 8
    addi x12, x0, 0
    addi x13, x0, 0
init:
    slli x14, x12, 2
    add  x15, x10, x14
    addi x16, x12, 7
    sw   x16, 0(x15)
    addi x12, x12, 1
    blt  x12, x11, init
    addi x12, x0, 0
sumloop:
    slli x14, x12, 2
    add  x15, x10, x14
    lw   x17, 0(x15)
    lw   x18, 4(x15)
    add  x13, x13, x17
    add  x13, x13, x18
    addi x12, x12, 1
    blt  x12, x11, sumloop
    sw   x13, 64(x10)
)";

// c[i] = a[i] + b[i]
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

// Dot product with a multiply.
constexpr const char *kDotProduct = R"(
    lui  x10, 2
    addi x11, x0, 8
    addi x12, x0, 0
    addi x13, x0, 0
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
dot:
    slli x14, x12, 2
    add  x15, x10, x14
    lw   x18, 0(x15)
    lw   x19, 32(x15)
    mul  x20, x18, x19
    add  x13, x13, x20
    addi x12, x12, 1
    blt  x12, x11, dot
    sw   x13, 64(x10)
)";

// Pointer chasing: every load needs the previous load, so there is nothing to interleave.
constexpr const char *kPointerChase = R"(
    lui  x10, 2
    addi x5, x10, 8
    sw   x5, 0(x10)
    addi x6, x10, 16
    sw   x6, 8(x10)
    addi x7, x10, 24
    sw   x7, 16(x10)
    lw   x1, 0(x10)
    lw   x2, 0(x1)
    lw   x3, 0(x2)
    sw   x3, 64(x10)
)";

// Pads three NOPs after every instruction, which is correct on every pipeline variant. This is
// what code written for a processor without hazard detection looks like when padded carelessly.
std::string padAfterEveryInstruction(const std::string &source)
{
    static const std::regex instruction(R"(^\s*(?:[A-Za-z_.$][A-Za-z0-9_.$]*\s*:\s*)*[a-z])");
    std::string padded;
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line))
    {
        padded += line + "\n";
        if (std::regex_search(line, instruction))
        {
            padded += "    addi x0, x0, 0\n    addi x0, x0, 0\n    addi x0, x0, 0\n";
        }
    }
    return padded;
}

struct Program
{
    const char *name;
    const char *source;
};

const Program kPrograms[] = {{"array_sum", kArraySum},
                             {"vector_add", kVectorAdd},
                             {"dot_product", kDotProduct},
                             {"pointer_chase", kPointerChase}};

struct Cycles
{
    unsigned before{0};
    unsigned after{0};
};

// Runs the original and the suggestion on Vm and checks both against the reference.
template <typename Vm>
Cycles checkVariant(const char *programName, const std::string &original,
                    const std::string &suggested, const Snapshot &reference, bool mustNotBeSlower)
{
    const RunResult before = runOn<Vm>(assembleSource(original));
    const RunResult after = runOn<Vm>(assembleSource(suggested));
    EXPECT_TRUE(before.snapshot == reference) << programName << ": original differs from reference";
    EXPECT_TRUE(after.snapshot == reference) << programName << ": suggestion changed the result";
    if (mustNotBeSlower)
    {
        EXPECT_LE(after.cycles, before.cycles) << programName << ": suggestion is slower";
    }
    return {before.cycles, after.cycles};
}
} // namespace

TEST(OptimizerSourceSimulation, ReferenceResultsAreWhatTheProgramsShouldCompute)
{
    setupVmStateDirectory();
    EXPECT_EQ(runOn<RVSSProcessor>(assembleSource(kArraySum)).snapshot.gprs[13], 161u);
    EXPECT_EQ(runOn<RVSSProcessor>(assembleSource(kDotProduct)).snapshot.gprs[13], 3768u);
    EXPECT_EQ(runOn<RVSSProcessor>(assembleSource(kPointerChase)).snapshot.gprs[3], 8216u);
}

TEST(OptimizerSourceSimulation, SuggestionsForHazardDetectingVariantsKeepResultsAndAreNotSlower)
{
    setupVmStateDirectory();
    for (const Program &program : kPrograms)
    {
        const Snapshot reference =
            runOn<RVSSProcessor>(assembleSource(program.source)).snapshot;

        const OptimizeResult noForwarding = optimize(program.source, PipelineVariant::H_NF);
        checkVariant<RV5StageProcessorHNF>(program.name, program.source, noForwarding.source,
                                           reference, true);

        const OptimizeResult forwarding = optimize(program.source, PipelineVariant::H_F);
        checkVariant<RV5StageProcessorHF>(program.name, program.source, forwarding.source,
                                          reference, true);
    }
}

TEST(OptimizerSourceSimulation, SuggestionsForNoHazardVariantsStayCorrectAndTurnPaddingIntoSavings)
{
    setupVmStateDirectory();
    int savedNoForwarding = 0;
    int savedForwarding = 0;
    for (const Program &program : kPrograms)
    {
        const std::string padded = padAfterEveryInstruction(program.source);
        const Snapshot reference = runOn<RVSSProcessor>(assembleSource(padded)).snapshot;

        const OptimizeResult noForwarding =
            optimize(padded, PipelineVariant::NH_NF, /*dropNops=*/true);
        const Cycles nhNf = checkVariant<RV5StageProcessorNHNF>(program.name, padded,
                                                                noForwarding.source, reference, true);
        savedNoForwarding += static_cast<int>(nhNf.before) - static_cast<int>(nhNf.after);

        const OptimizeResult forwarding = optimize(padded, PipelineVariant::NH_F, /*dropNops=*/true);
        const Cycles nhF = checkVariant<RV5StageProcessorNHF>(program.name, padded, forwarding.source,
                                                              reference, true);
        savedForwarding += static_cast<int>(nhF.before) - static_cast<int>(nhF.after);
    }

    // Generous hand-written padding is turned into real savings.
    EXPECT_GT(savedNoForwarding, 0);
    EXPECT_GT(savedForwarding, 0);
}
