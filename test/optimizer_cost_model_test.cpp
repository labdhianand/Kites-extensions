#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "optimizer/cost_model.h"
#include "optimizer/timeline.h"
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

CostModel modelFor(PipelineVariant variant)
{
    CostModel costModel;
    costModel.variant = variant;
    return costModel;
}

// Instruction indices 0..n-1, i.e. the program in its written order.
std::vector<size_t> writtenOrder(const ProgramModel &model)
{
    std::vector<size_t> order(model.instructions.size());
    for (size_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    return order;
}

Timeline timelineOf(const std::string &source, PipelineVariant variant)
{
    const ProgramModel model = buildProgramModel(assembleSource(source));
    return estimateTimeline(model, modelFor(variant), writtenOrder(model));
}
} // namespace

// ---------------------------------------------------------------------------
// The measured gap table
// ---------------------------------------------------------------------------
TEST(OptimizerCostModel, MinimumGapsMatchTheMeasuredTable)
{
    const ProgramModel model = buildProgramModel(assembleSource("add x3, x1, x2\n lw x5, 0(x6)\n"));
    const InstrInfo &alu = model.instructions[0];
    const InstrInfo &load = model.instructions[1];

    EXPECT_EQ(modelFor(PipelineVariant::NH_NF).minGap(alu), 3);
    EXPECT_EQ(modelFor(PipelineVariant::NH_NF).minGap(load), 3);
    EXPECT_EQ(modelFor(PipelineVariant::H_NF).minGap(alu), 3);
    EXPECT_EQ(modelFor(PipelineVariant::H_NF).minGap(load), 3);
    EXPECT_EQ(modelFor(PipelineVariant::NH_F).minGap(alu), 1);
    EXPECT_EQ(modelFor(PipelineVariant::NH_F).minGap(load), 2);
    EXPECT_EQ(modelFor(PipelineVariant::H_F).minGap(alu), 1);
    EXPECT_EQ(modelFor(PipelineVariant::H_F).minGap(load), 2);
}

TEST(OptimizerCostModel, VariantsReportWhoEnforcesTheGap)
{
    EXPECT_FALSE(modelFor(PipelineVariant::NH_NF).detectsHazards());
    EXPECT_TRUE(modelFor(PipelineVariant::H_NF).detectsHazards());
    EXPECT_FALSE(modelFor(PipelineVariant::NH_F).detectsHazards());
    EXPECT_TRUE(modelFor(PipelineVariant::H_F).detectsHazards());

    EXPECT_FALSE(modelFor(PipelineVariant::NH_NF).hasForwarding());
    EXPECT_FALSE(modelFor(PipelineVariant::H_NF).hasForwarding());
    EXPECT_TRUE(modelFor(PipelineVariant::NH_F).hasForwarding());
    EXPECT_TRUE(modelFor(PipelineVariant::H_F).hasForwarding());
}

// ---------------------------------------------------------------------------
// Timeline
// ---------------------------------------------------------------------------
TEST(OptimizerTimeline, EmptySequenceUsesNoSlots)
{
    const ProgramModel model;
    const Timeline timeline = estimateTimeline(model, modelFor(PipelineVariant::H_F), {});
    EXPECT_EQ(timeline.slotCount, 0);
    EXPECT_EQ(timeline.bubbleCount, 0);
}

TEST(OptimizerTimeline, LoadUseCostsOneBubbleWithForwarding)
{
    const Timeline timeline = timelineOf("lw x1, 0(x2)\n add x3, x1, x1\n", PipelineVariant::H_F);
    EXPECT_EQ(timeline.bubbleCount, 1);
    EXPECT_EQ(timeline.slotCount, 3);
    EXPECT_EQ(timeline.violationCount, 1);
}

TEST(OptimizerTimeline, AluResultNeedsNoBubbleWithForwarding)
{
    const Timeline timeline = timelineOf("addi x1, x0, 5\n add x3, x1, x1\n", PipelineVariant::H_F);
    EXPECT_EQ(timeline.bubbleCount, 0);
    EXPECT_EQ(timeline.violationCount, 0);
}

TEST(OptimizerTimeline, IndependentInstructionBetweenRemovesTheLoadUseBubble)
{
    const Timeline timeline =
        timelineOf("lw x1, 0(x2)\n addi x5, x0, 1\n add x3, x1, x1\n", PipelineVariant::H_F);
    EXPECT_EQ(timeline.bubbleCount, 0);
    EXPECT_EQ(timeline.slotCount, 3);
}

TEST(OptimizerTimeline, WithoutForwardingBubblesShrinkWithDistance)
{
    EXPECT_EQ(
        timelineOf("addi x1, x0, 1\n add x2, x1, x1\n", PipelineVariant::H_NF).bubbleCount, 2);
    EXPECT_EQ(timelineOf("addi x1, x0, 1\n addi x5, x0, 1\n add x2, x1, x1\n",
                         PipelineVariant::H_NF)
                  .bubbleCount,
              1);
    EXPECT_EQ(timelineOf("addi x1, x0, 1\n addi x5, x0, 1\n addi x6, x0, 1\n add x2, x1, x1\n",
                         PipelineVariant::H_NF)
                  .bubbleCount,
              0);
}

TEST(OptimizerTimeline, ABubbleDelaysLaterInstructionsAndCountsTowardTheirDistance)
{
    // The load-use bubble pushes the second add out by one slot; the third add then reads an
    // ALU result from the adjacent instruction, which needs no bubble with forwarding.
    const Timeline timeline = timelineOf(
        "lw x1, 0(x2)\n add x2, x1, x1\n add x3, x2, x2\n", PipelineVariant::H_F);
    EXPECT_EQ(timeline.bubbleCount, 1);
    EXPECT_EQ(timeline.slotCount, 4);
}

TEST(OptimizerTimeline, TheSlowestOfSeveralProducersDecides)
{
    // x2 is adjacent to the consumer (needs 3 slots), x1 is two ahead (needs 3 slots): the
    // consumer waits for x2, so it issues at slot 4 with two bubbles.
    const Timeline timeline = timelineOf(
        "addi x1, x0, 1\n addi x2, x0, 2\n add x3, x1, x2\n", PipelineVariant::H_NF);
    EXPECT_EQ(timeline.bubbleCount, 2);
    EXPECT_EQ(timeline.issueSlot.back(), 4);
}

TEST(OptimizerTimeline, OnlyTheLatestWriterOfARegisterMatters)
{
    const Timeline timeline = timelineOf(
        "addi x1, x0, 1\n addi x1, x0, 2\n add x2, x1, x1\n", PipelineVariant::H_NF);
    EXPECT_EQ(timeline.bubbleCount, 2); // measured from the second addi, which is adjacent
}

TEST(OptimizerTimeline, ZeroRegisterNeverCausesABubble)
{
    const Timeline timeline = timelineOf("addi x0, x1, 1\n add x2, x0, x0\n", PipelineVariant::H_NF);
    EXPECT_EQ(timeline.bubbleCount, 0);
}

TEST(OptimizerTimeline, FloatingPointLoadsFollowTheLoadRule)
{
    const Timeline timeline =
        timelineOf("flw f1, 0(x2)\n fadd.s f2, f1, f1\n", PipelineVariant::H_F);
    EXPECT_EQ(timeline.bubbleCount, 1);
}

TEST(OptimizerTimeline, WithoutHazardDetectionTheBubblesAreRequiredPadding)
{
    const Timeline timeline =
        timelineOf("addi x1, x0, 1\n add x2, x1, x1\n", PipelineVariant::NH_NF);
    EXPECT_EQ(timeline.violationCount, 1); // as written, the program would read a stale value
    EXPECT_EQ(timeline.bubbleCount, 2);    // two NOPs would fix it
    EXPECT_EQ(timeline.bubblesBefore[1], 2);
}

TEST(OptimizerTimeline, ExplicitNopsCountAsRealInstructionsAndSatisfyTheGap)
{
    const Timeline timeline = timelineOf(
        "addi x1, x0, 1\n addi x0, x0, 0\n addi x0, x0, 0\n add x2, x1, x1\n",
        PipelineVariant::NH_NF);
    EXPECT_EQ(timeline.violationCount, 0);
    EXPECT_EQ(timeline.bubbleCount, 0);
    EXPECT_EQ(timeline.slotCount, 4);
}

// ---------------------------------------------------------------------------
// Validation against the simulator
//
// The model is only worth anything if it predicts what the simulator does. Random straight-line
// programs are run on all four pipeline variants:
//  - with hazard detection, measured cycles must equal the model's slots plus a constant that
//    covers pipeline fill and drain;
//  - without it, a program padded with exactly the NOPs the model asks for must compute the same
//    result as on the single-cycle processor (and the unpadded program should often not).
// ---------------------------------------------------------------------------
namespace
{
constexpr size_t kDefaultValidationPrograms = 6;

// A program of independent instructions, used to measure the constant pipeline fill/drain cost.
unsigned fillAndDrainCycles(const std::function<unsigned(const AssembledProgram &)> &cyclesOf)
{
    std::string source;
    for (int i = 0; i < 12; ++i)
    {
        source += "addi x" + std::to_string(20 + i) + ", x0, " + std::to_string(i + 1) + "\n";
    }
    return cyclesOf(assembleSource(source)) - 12;
}

} // namespace

TEST(OptimizerCostModelValidation, HazardDetectingVariantsMatchMeasuredCycles)
{
    setupVmStateDirectory();
    std::mt19937 rng(7);
    const size_t programCount =
        programCountFromEnvironment("KITES_VALIDATE_PROGRAMS", kDefaultValidationPrograms);

    const unsigned fillHNF = fillAndDrainCycles([](const AssembledProgram &p)
                                                { return runOn<RV5StageProcessorHNF>(p).cycles; });
    const unsigned fillHF = fillAndDrainCycles([](const AssembledProgram &p)
                                               { return runOn<RV5StageProcessorHF>(p).cycles; });

    for (size_t i = 0; i < programCount; ++i)
    {
        const std::string source = randomProgram(rng, 16, /*includeFloatingPoint=*/false);
        SCOPED_TRACE("program " + std::to_string(i) + ":\n" + source);
        const AssembledProgram program = assembleSource(source);
        const ProgramModel model = buildProgramModel(program);
        const Snapshot reference = runOn<RVSSProcessor>(program).snapshot;

        const RunResult hnf = runOn<RV5StageProcessorHNF>(program);
        const Timeline hnfTimeline =
            estimateTimeline(model, modelFor(PipelineVariant::H_NF), writtenOrder(model));
        EXPECT_EQ(hnf.cycles, hnfTimeline.slotCount + fillHNF) << "H_NF cycle estimate";
        EXPECT_TRUE(hnf.snapshot == reference) << "H_NF result";

        const RunResult hf = runOn<RV5StageProcessorHF>(program);
        const Timeline hfTimeline =
            estimateTimeline(model, modelFor(PipelineVariant::H_F), writtenOrder(model));
        EXPECT_EQ(hf.cycles, hfTimeline.slotCount + fillHF) << "H_F cycle estimate";
        EXPECT_TRUE(hf.snapshot == reference) << "H_F result";
    }
}

TEST(OptimizerCostModelValidation, PaddingTheModelsBubblesMakesNoHazardVariantsCorrect)
{
    setupVmStateDirectory();
    std::mt19937 rng(11);
    const size_t programCount =
        programCountFromEnvironment("KITES_VALIDATE_PROGRAMS", kDefaultValidationPrograms);
    size_t unpaddedWrongCount = 0;

    for (size_t i = 0; i < programCount; ++i)
    {
        const std::string source = randomProgram(rng, 16, /*includeFloatingPoint=*/false);
        SCOPED_TRACE("program " + std::to_string(i) + ":\n" + source);
        const AssembledProgram program = assembleSource(source);
        const ProgramModel model = buildProgramModel(program);
        const Snapshot reference = runOn<RVSSProcessor>(program).snapshot;

        const Timeline nhNf =
            estimateTimeline(model, modelFor(PipelineVariant::NH_NF), writtenOrder(model));
        EXPECT_TRUE(runOn<RV5StageProcessorNHNF>(paddedWithNops(program, nhNf)).snapshot == reference)
            << "NH_NF padded";
        if (!(runOn<RV5StageProcessorNHNF>(program).snapshot == reference))
        {
            ++unpaddedWrongCount;
        }

        const Timeline nhF =
            estimateTimeline(model, modelFor(PipelineVariant::NH_F), writtenOrder(model));
        EXPECT_TRUE(runOn<RV5StageProcessorNHF>(paddedWithNops(program, nhF)).snapshot == reference)
            << "NH_F padded";
    }

    // Guards the test itself: without the padding the hazards really do corrupt results.
    EXPECT_GT(unpaddedWrongCount, programCount / 2);
}

// ---------------------------------------------------------------------------
// Memory latency
// ---------------------------------------------------------------------------
TEST(OptimizerCostModel, ALoadsGapGrowsByThePlannedLatencyWhereTheHardwareCanWait)
{
    const ProgramModel model = buildProgramModel(assembleSource("add x3, x1, x2\n lw x5, 0(x6)\n"));
    const InstrInfo &alu = model.instructions[0];
    const InstrInfo &load = model.instructions[1];

    CostModel forwarding = modelFor(PipelineVariant::H_F);
    forwarding.loadExtraLatency = 4;
    EXPECT_EQ(forwarding.minGap(load), 6);
    EXPECT_EQ(forwarding.minGap(alu), 1);
    EXPECT_EQ(forwarding.maxGap(), 6);

    CostModel noForwarding = modelFor(PipelineVariant::H_NF);
    noForwarding.loadExtraLatency = 4;
    EXPECT_EQ(noForwarding.minGap(load), 7);
    EXPECT_EQ(noForwarding.minGap(alu), 3);

    // Without hazard detection slow loads block the whole pipeline, which no order can hide.
    CostModel noHazard = modelFor(PipelineVariant::NH_F);
    noHazard.loadExtraLatency = 4;
    EXPECT_EQ(noHazard.minGap(load), 2);
    EXPECT_EQ(noHazard.maxGap(), 2);
}

namespace
{
MemoryTiming uniformLatency(unsigned extra, bool nonBlocking)
{
    MemoryTiming timing;
    timing.enabled = true;
    timing.nonBlocking = nonBlocking;
    timing.l1HitCycles = timing.l2HitCycles = timing.memoryCycles = extra;
    return timing;
}

template <typename Vm> unsigned cyclesWithTiming(const AssembledProgram &program, const MemoryTiming &timing)
{
    return runOn<Vm>(program, [&](Vm &vm) { vm.memory_timing_ = timing; }).cycles;
}

size_t loadCount(const ProgramModel &model)
{
    size_t count = 0;
    for (const InstrInfo &info : model.instructions)
    {
        count += info.klass == InstrClass::Load ? 1 : 0;
    }
    return count;
}

template <typename Vm>
void checkLatencyModel(const char *name, PipelineVariant variant, const AssembledProgram &program,
                       const ProgramModel &model, unsigned fill, unsigned extra)
{
    // Non-blocking: only consumers wait, exactly as the timeline predicts with the latency planned.
    CostModel planned = modelFor(variant);
    planned.loadExtraLatency = static_cast<int>(extra);
    const Timeline nonBlocking = estimateTimeline(model, planned, writtenOrder(model));
    EXPECT_EQ(cyclesWithTiming<Vm>(program, uniformLatency(extra, true)),
              nonBlocking.slotCount + fill)
        << name << " non-blocking";

    // Blocking: every load costs its latency whatever the order.
    const Timeline plain = estimateTimeline(model, modelFor(variant), writtenOrder(model));
    EXPECT_EQ(cyclesWithTiming<Vm>(program, uniformLatency(extra, false)),
              plain.slotCount + fill + extra * loadCount(model))
        << name << " blocking";
}
} // namespace

TEST(OptimizerCostModelValidation, TheLatencyModelPredictsMeasuredCycles)
{
    setupVmStateDirectory();
    std::mt19937 rng(23);
    const size_t programCount =
        programCountFromEnvironment("KITES_VALIDATE_PROGRAMS", kDefaultValidationPrograms);
    const unsigned fillHNF = fillAndDrainCycles([](const AssembledProgram &p)
                                                { return runOn<RV5StageProcessorHNF>(p).cycles; });
    const unsigned fillHF = fillAndDrainCycles([](const AssembledProgram &p)
                                               { return runOn<RV5StageProcessorHF>(p).cycles; });

    for (size_t i = 0; i < programCount; ++i)
    {
        const std::string source = randomProgram(rng, 16, /*includeFloatingPoint=*/false);
        SCOPED_TRACE("program " + std::to_string(i) + ":\n" + source);
        const AssembledProgram program = assembleSource(source);
        const ProgramModel model = buildProgramModel(program);
        for (unsigned extra : {1u, 4u})
        {
            checkLatencyModel<RV5StageProcessorHNF>("H_NF", PipelineVariant::H_NF, program, model,
                                                    fillHNF, extra);
            checkLatencyModel<RV5StageProcessorHF>("H_F", PipelineVariant::H_F, program, model,
                                                   fillHF, extra);
        }
    }
}
