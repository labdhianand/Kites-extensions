#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "optimizer/dependency_graph.h"
#include "optimizer/scheduler.h"
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

BlockSchedule scheduleFirstBlock(const std::string &source, PipelineVariant variant,
                                 ScheduleOptions options = {})
{
    const ProgramModel model = buildProgramModel(assembleSource(source));
    return scheduleBlock(model, model.blocks.at(0), modelFor(variant), options);
}

using Order = std::vector<size_t>;

// Two loads whose results are used straight away: the textbook case.
constexpr const char *kLoadUseProgram = R"(
    lw  x1, 0(x10)
    add x2, x1, x1
    lw  x3, 4(x10)
    add x4, x3, x3
)";
} // namespace

// ---------------------------------------------------------------------------
// Hand-checked cases
// ---------------------------------------------------------------------------
TEST(OptimizerScheduler, HoistingTheSecondLoadRemovesBothLoadUseStallsWithForwarding)
{
    const BlockSchedule schedule = scheduleFirstBlock(kLoadUseProgram, PipelineVariant::H_F);
    EXPECT_TRUE(schedule.changed);
    EXPECT_EQ(schedule.unitOrder, (Order{0, 2, 1, 3}));
    EXPECT_EQ(schedule.originalSlots, 6); // 4 instructions + 2 load-use bubbles
    EXPECT_EQ(schedule.scheduledSlots, 4);
    EXPECT_EQ(schedule.scheduledBubbles, 0);
    EXPECT_EQ(schedule.savedSlots(), 2);
}

TEST(OptimizerScheduler, WithoutForwardingTheSameReorderingSavesThreeSlots)
{
    const BlockSchedule schedule = scheduleFirstBlock(kLoadUseProgram, PipelineVariant::H_NF);
    EXPECT_TRUE(schedule.changed);
    EXPECT_EQ(schedule.unitOrder, (Order{0, 2, 1, 3}));
    EXPECT_EQ(schedule.originalSlots, 8);
    EXPECT_EQ(schedule.scheduledSlots, 5);
    EXPECT_EQ(schedule.scheduledBubbles, 1);
}

TEST(OptimizerScheduler, IndependentInstructionsAreLeftAlone)
{
    const BlockSchedule schedule =
        scheduleFirstBlock("add x2, x3, x4\n add x5, x6, x7\n add x8, x9, x10\n", PipelineVariant::H_NF);
    EXPECT_FALSE(schedule.changed);
    EXPECT_EQ(schedule.unitOrder, (Order{0, 1, 2}));
    EXPECT_EQ(schedule.savedSlots(), 0);
    EXPECT_TRUE(schedule.reason.empty());
}

TEST(OptimizerScheduler, ADependencyChainWithNothingToInterleaveCannotImprove)
{
    const BlockSchedule schedule =
        scheduleFirstBlock("lw x1, 0(x10)\n add x2, x1, x1\n", PipelineVariant::H_F);
    EXPECT_FALSE(schedule.changed);
    EXPECT_EQ(schedule.savedSlots(), 0);
}

TEST(OptimizerScheduler, TheBranchStaysLastAndIndependentWorkFillsTheGap)
{
    const BlockSchedule schedule = scheduleFirstBlock(R"(
        lw   x1, 0(x10)
        add  x2, x1, x1
        addi x5, x0, 1
        beq  x2, x0, done
    done:
        addi x6, x0, 1
    )",
                                                      PipelineVariant::H_F);
    EXPECT_TRUE(schedule.changed);
    EXPECT_EQ(schedule.unitOrder, (Order{0, 2, 1, 3}));
    EXPECT_EQ(schedule.unitOrder.back(), 3u);
    EXPECT_EQ(schedule.savedSlots(), 1);
}

TEST(OptimizerScheduler, LoadsStayAfterTheStoresTheyMightDependOn)
{
    const BlockSchedule schedule = scheduleFirstBlock(R"(
        sw   x1, 0(x10)
        lw   x2, 0(x10)
        add  x3, x2, x2
        addi x5, x0, 1
    )",
                                                      PipelineVariant::H_F);
    EXPECT_TRUE(schedule.changed);
    EXPECT_EQ(schedule.unitOrder, (Order{0, 1, 3, 2}));
    EXPECT_EQ(schedule.savedSlots(), 1);
}

TEST(OptimizerScheduler, EcallEndsTheScheduledBlock)
{
    const BlockSchedule schedule = scheduleFirstBlock(R"(
        lw   x1, 0(x10)
        add  x2, x1, x1
        addi x5, x0, 1
        addi a7, x0, 10
        ecall
    )",
                                                      PipelineVariant::H_F);
    EXPECT_EQ(schedule.unitOrder.back(), 4u);
}

TEST(OptimizerScheduler, FloatingPointBlocksAreLeftUntouchedWithAReason)
{
    const BlockSchedule schedule = scheduleFirstBlock(R"(
        flw    f1, 0(x10)
        fadd.s f2, f1, f1
        addi   x5, x0, 1
    )",
                                                      PipelineVariant::H_F);
    EXPECT_FALSE(schedule.changed);
    EXPECT_EQ(schedule.unitOrder, (Order{0, 1, 2}));
    EXPECT_NE(schedule.reason.find("floating-point"), std::string::npos);
}

TEST(OptimizerScheduler, HandWrittenNopPaddingBecomesUsefulWorkWhenDropped)
{
    const std::string source = R"(
        lw   x1, 0(x10)
        addi x0, x0, 0
        addi x0, x0, 0
        add  x2, x1, x1
        addi x5, x0, 1
        addi x6, x0, 2
    )";
    // Without hazard detection the two NOPs are what makes `add` read the loaded value. Nothing
    // runs after this block, so nothing needs protecting on the way out.
    ScheduleOptions drop;
    drop.dropNops = true;
    drop.liveOut = RegisterSet();
    const BlockSchedule schedule = scheduleFirstBlock(source, PipelineVariant::NH_NF, drop);
    EXPECT_TRUE(schedule.changed);
    EXPECT_EQ(schedule.unitOrder, (Order{0, 4, 5, 3}));
    EXPECT_EQ(schedule.droppedUnits, (Order{1, 2}));
    EXPECT_EQ(schedule.originalSlots, 6);
    EXPECT_EQ(schedule.scheduledSlots, 4);
    EXPECT_EQ(schedule.scheduledBubbles, 0);

    // Treating the NOPs as ordinary instructions finds nothing better: they are already doing the
    // job of filler.
    const BlockSchedule kept = scheduleFirstBlock(source, PipelineVariant::NH_NF);
    EXPECT_FALSE(kept.changed);
}

TEST(OptimizerScheduler, KeepsPaddingThatProtectsTheNextBlockOnNoHazardVariants)
{
    // The two trailing NOPs exist so that the next block can read x2 safely. They look like
    // droppable padding from inside this block, but removing them would hand the next block a
    // stale value.
    const std::string source = R"(
        lw   x1, 0(x10)
        addi x0, x0, 0
        addi x0, x0, 0
        add  x2, x1, x1
        addi x0, x0, 0
        addi x0, x0, 0
    next:
        add  x6, x2, x2
    )";
    ScheduleOptions drop;
    drop.dropNops = true;
    const BlockSchedule schedule = scheduleFirstBlock(source, PipelineVariant::NH_NF, drop);
    EXPECT_FALSE(schedule.changed);
    EXPECT_TRUE(schedule.droppedUnits.empty());
}

TEST(OptimizerScheduler, MovingAConsumerToTheStartOfABlockIsRefusedOnNoHazardVariants)
{
    // `add x6, x2, x2` reads x2, which the previous block may have written just before. A later
    // instruction depends on x6, so the scheduler wants to hoist it to the very top, but that would
    // shrink the distance from the previous block's producer and the hardware will not stall to
    // fix it.
    const std::string source = R"(
        addi x5, x0, 1
        addi x8, x0, 3
        add  x6, x2, x2
        add  x7, x6, x6
    )";
    EXPECT_FALSE(scheduleFirstBlock(source, PipelineVariant::NH_NF).changed);

    // With hazard detection the hardware stalls if the previous block's producer is too close, so
    // the same hoist is fine.
    EXPECT_TRUE(scheduleFirstBlock(source, PipelineVariant::H_NF).changed);
}

TEST(OptimizerScheduler, OnlyRegistersThatAreReadAfterTheBlockNeedProtecting)
{
    const std::string source = R"(
        lw   x1, 0(x10)
        addi x0, x0, 0
        addi x0, x0, 0
        add  x2, x1, x1
        addi x5, x0, 1
        addi x6, x0, 2
    )";
    ScheduleOptions options;
    options.dropNops = true;

    // x2 is the register whose last write would end up too close to the end of the block.
    RegisterSet readsX2;
    readsX2.set(2);
    options.liveOut = readsX2;
    EXPECT_FALSE(scheduleFirstBlock(source, PipelineVariant::NH_NF, options).changed);

    // Nothing, or an unrelated register, is read afterwards, so the same reordering is safe.
    options.liveOut = RegisterSet();
    EXPECT_TRUE(scheduleFirstBlock(source, PipelineVariant::NH_NF, options).changed);
    RegisterSet readsX9;
    readsX9.set(9);
    options.liveOut = readsX9;
    EXPECT_TRUE(scheduleFirstBlock(source, PipelineVariant::NH_NF, options).changed);

    // With hazard detection the hardware stalls the reader, so liveness is irrelevant.
    options.liveOut = readsX2;
    EXPECT_TRUE(scheduleFirstBlock(source, PipelineVariant::H_NF, options).changed);
}

TEST(OptimizerScheduler, ReportsPaddingTheModelNeedsBeforeEachUnit)
{
    const BlockSchedule schedule = scheduleFirstBlock(kLoadUseProgram, PipelineVariant::H_NF);
    ASSERT_EQ(schedule.bubblesBefore.size(), schedule.unitOrder.size());
    int total = 0;
    for (int bubbles : schedule.bubblesBefore)
    {
        total += bubbles;
    }
    EXPECT_EQ(total, schedule.scheduledBubbles);
}

// ---------------------------------------------------------------------------
// Properties over many random blocks (no simulator needed)
// ---------------------------------------------------------------------------
namespace
{
// Every dependency between two units that were kept must still point forward.
bool respectsGraphIgnoring(const DependencyGraph &graph, const BlockSchedule &schedule)
{
    std::vector<size_t> position(graph.nodeCount, graph.nodeCount);
    for (size_t slot = 0; slot < schedule.unitOrder.size(); ++slot)
    {
        position[schedule.unitOrder[slot]] = slot;
    }
    for (const Dependency &edge : graph.edges)
    {
        const bool fromKept = position[edge.from] != graph.nodeCount;
        const bool toKept = position[edge.to] != graph.nodeCount;
        if (fromKept && toKept && position[edge.from] >= position[edge.to])
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST(OptimizerSchedulerProperties, EveryScheduleIsLegalAndNeverWorse)
{
    std::mt19937 rng(424242);
    const PipelineVariant variants[] = {PipelineVariant::NH_NF, PipelineVariant::H_NF,
                                        PipelineVariant::NH_F, PipelineVariant::H_F};
    size_t improvedCount = 0;
    size_t totalCount = 0;

    for (int i = 0; i < 150; ++i)
    {
        // Half of the programs end in a branch so the terminator rule is exercised as well.
        std::string source = randomProgram(rng, 16, /*includeFloatingPoint=*/false);
        if (i % 2 == 1)
        {
            source += "beq x5, x6, done\n done:\n addi x0, x0, 0\n";
        }
        const ProgramModel model = buildProgramModel(assembleSource(source));
        const BasicBlock &block = model.blocks.at(0);
        const DependencyGraph graph = buildDependencyGraph(model, block);

        for (PipelineVariant variant : variants)
        {
            for (bool dropNops : {false, true})
            {
                ScheduleOptions options;
                options.dropNops = dropNops;
                const BlockSchedule schedule = scheduleBlock(model, block, modelFor(variant), options);
                SCOPED_TRACE("program " + std::to_string(i) + ":\n" + source);

                EXPECT_LE(schedule.scheduledSlots, schedule.originalSlots);
                EXPECT_GE(schedule.savedSlots(), 0);
                EXPECT_TRUE(respectsGraphIgnoring(graph, schedule));
                EXPECT_EQ(schedule.unitOrder.size() + schedule.droppedUnits.size(),
                          block.unitCount);

                if (block.hasTerminator && schedule.droppedUnits.empty())
                {
                    EXPECT_EQ(schedule.unitOrder.back(), block.unitCount - 1)
                        << "the branch must stay last";
                }
                if (!schedule.changed)
                {
                    EXPECT_EQ(schedule.savedSlots(), 0);
                }
                ++totalCount;
                improvedCount += schedule.changed ? 1 : 0;
            }
        }
    }

    // The property is only interesting if the scheduler actually does something.
    EXPECT_GT(improvedCount, totalCount / 4);
}

// ---------------------------------------------------------------------------
// End to end on the simulator
//
// The scheduler's output is run on the real processor models:
//  - the reordered program must compute exactly what the original does;
//  - with hazard detection, measured cycles must match the model and not exceed the original's;
//  - without it, the reordered program padded with the NOPs the model asks for must be correct.
// ---------------------------------------------------------------------------
namespace
{
constexpr size_t kDefaultSimulatedPrograms = 5;

template <typename Vm> unsigned fillAndDrain()
{
    std::string source;
    for (int i = 0; i < 12; ++i)
    {
        source += "addi x" + std::to_string(20 + i) + ", x0, " + std::to_string(i + 1) + "\n";
    }
    return runOn<Vm>(assembleSource(source)).cycles - 12;
}

// Every instruction of the program in its written order.
std::vector<size_t> allInstructions(const ProgramModel &model)
{
    std::vector<size_t> instructions(model.instructions.size());
    for (size_t i = 0; i < instructions.size(); ++i)
    {
        instructions[i] = i;
    }
    return instructions;
}

template <typename Vm>
void checkHazardDetectingVariant(const char *name, PipelineVariant variant, unsigned fill,
                                 const AssembledProgram &program, const ProgramModel &model,
                                 const BasicBlock &block, const Snapshot &reference,
                                 int &savedTotal)
{
    const BlockSchedule schedule = scheduleBlock(model, block, modelFor(variant));
    const RunResult original = runOn<Vm>(program);
    const RunResult reordered = runOn<Vm>(withOrder(program, model, block, schedule.unitOrder));

    EXPECT_TRUE(reordered.snapshot == reference) << name << ": reordering changed the result";
    EXPECT_EQ(reordered.cycles, schedule.scheduledSlots + fill) << name << ": cycle estimate";
    EXPECT_LE(reordered.cycles, original.cycles) << name << ": got slower";
    savedTotal += static_cast<int>(original.cycles) - static_cast<int>(reordered.cycles);
}

template <typename Vm>
void checkNoHazardVariant(const char *name, PipelineVariant variant, const AssembledProgram &program,
                          const ProgramModel &model, const BasicBlock &block,
                          const Snapshot &reference)
{
    const BlockSchedule schedule = scheduleBlock(model, block, modelFor(variant));
    const AssembledProgram reordered = withOrder(program, model, block, schedule.unitOrder);
    const ProgramModel reorderedModel = buildProgramModel(reordered);
    const Timeline timeline =
        estimateTimeline(reorderedModel, modelFor(variant), allInstructions(reorderedModel));
    EXPECT_TRUE(runOn<Vm>(paddedWithNops(reordered, timeline)).snapshot == reference)
        << name << ": padded reordering is wrong";
}
} // namespace

TEST(OptimizerSchedulerSimulation, ReorderedProgramsAreCorrectAndNotSlowerOnEveryVariant)
{
    setupVmStateDirectory();
    std::mt19937 rng(99);
    const size_t programCount =
        programCountFromEnvironment("KITES_SCHEDULE_PROGRAMS", kDefaultSimulatedPrograms);

    const unsigned fillHNF = fillAndDrain<RV5StageProcessorHNF>();
    const unsigned fillHF = fillAndDrain<RV5StageProcessorHF>();
    int savedHNF = 0;
    int savedHF = 0;

    for (size_t i = 0; i < programCount; ++i)
    {
        const std::string source = randomProgram(rng, 16, /*includeFloatingPoint=*/false);
        SCOPED_TRACE("program " + std::to_string(i) + ":\n" + source);
        const AssembledProgram program = assembleSource(source);
        const ProgramModel model = buildProgramModel(program);
        const BasicBlock &block = model.blocks.at(0);
        const Snapshot reference = runOn<RVSSProcessor>(program).snapshot;

        checkHazardDetectingVariant<RV5StageProcessorHNF>("H_NF", PipelineVariant::H_NF, fillHNF,
                                                          program, model, block, reference,
                                                          savedHNF);
        checkHazardDetectingVariant<RV5StageProcessorHF>("H_F", PipelineVariant::H_F, fillHF,
                                                         program, model, block, reference,
                                                         savedHF);
        checkNoHazardVariant<RV5StageProcessorNHNF>("NH_NF", PipelineVariant::NH_NF, program, model,
                                                    block, reference);
        checkNoHazardVariant<RV5StageProcessorNHF>("NH_F", PipelineVariant::NH_F, program, model,
                                                   block, reference);
    }

    // Reordering should actually pay off on the variants that stall.
    EXPECT_GT(savedHNF, 0);
    EXPECT_GE(savedHF, 0);
}

TEST(OptimizerSchedulerSimulation, DroppingHandWrittenPaddingKeepsResultsAndSavesCycles)
{
    setupVmStateDirectory();
    // Setup, then a load whose consumer is separated by hand-written NOPs, then independent work.
    const std::string source = R"(
        addi x10, x0, 256
        addi x0, x0, 0
        addi x0, x0, 0
        addi x7, x0, 77
        addi x0, x0, 0
        addi x0, x0, 0
        sw   x7, 0(x10)
        addi x0, x0, 0
        addi x0, x0, 0
        lw   x1, 0(x10)
        addi x0, x0, 0
        addi x0, x0, 0
        add  x2, x1, x1
        addi x5, x0, 1
        addi x6, x0, 2
    )";
    const AssembledProgram program = assembleSource(source);
    const ProgramModel model = buildProgramModel(program);
    const BasicBlock &block = model.blocks.at(0);

    ScheduleOptions drop;
    drop.dropNops = true;
    drop.liveOut = RegisterSet(); // this is the whole program, so nothing is read afterwards
    const BlockSchedule schedule = scheduleBlock(model, block, modelFor(PipelineVariant::NH_NF), drop);
    ASSERT_TRUE(schedule.changed);

    const AssembledProgram reordered = withOrder(program, model, block, schedule.unitOrder);
    const ProgramModel reorderedModel = buildProgramModel(reordered);
    const Timeline timeline = estimateTimeline(reorderedModel, modelFor(PipelineVariant::NH_NF),
                                               allInstructions(reorderedModel));
    const AssembledProgram padded = paddedWithNops(reordered, timeline);

    const RunResult before = runOn<RV5StageProcessorNHNF>(program);
    const RunResult after = runOn<RV5StageProcessorNHNF>(padded);

    EXPECT_EQ(before.snapshot.gprs[2], 154u); // the original is correct on this variant
    EXPECT_TRUE(after.snapshot == before.snapshot);
    EXPECT_LT(after.cycles, before.cycles);
}
