// Tests for the opt-in memory-latency model of the 5-stage pipelines (processor/memory_timing.h).

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string>

#include "optimizer_test_support.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rv5s/rv5s_processor_nh_f.h"
#include "processor/rv5s/rv5s_processor_nh_nf.h"
#include "utils/utils.h"

using namespace Kites;
using namespace optimizer_test;

namespace
{
MemoryTiming uniform(unsigned extra, bool nonBlocking)
{
    MemoryTiming timing;
    timing.enabled = true;
    timing.nonBlocking = nonBlocking;
    timing.l1HitCycles = timing.l2HitCycles = timing.memoryCycles = extra;
    return timing;
}

std::string nops(int count)
{
    std::string text;
    for (int i = 0; i < count; ++i)
    {
        text += "addi x0, x0, 0\n";
    }
    return text;
}

// A load whose result is read `distance` instructions later (1 = the very next instruction).
// Everything else is padded so that only the load matters, on every pipeline variant.
std::string loadThenUse(int distance, bool consumerReadsLoad = true)
{
    std::string text = "lui x10, 2\n" + nops(4) + "lw x5, 0(x10)\n";
    for (int i = 1; i < distance; ++i)
    {
        text += "addi x" + std::to_string(20 + i) + ", x0, " + std::to_string(i) + "\n";
    }
    text += consumerReadsLoad ? "add x6, x5, x5\n" : "add x6, x10, x10\n";
    return text + nops(4);
}

template <typename Vm> unsigned cycles(const std::string &source, const MemoryTiming &timing = {})
{
    return runOn<Vm>(assembleSource(source),
                     [&](Vm &vm) { vm.memory_timing_ = timing; })
        .cycles;
}

template <typename Vm> int extraCycles(const std::string &source, const MemoryTiming &timing)
{
    return static_cast<int>(cycles<Vm>(source, timing)) - static_cast<int>(cycles<Vm>(source));
}
} // namespace

class MemoryTimingTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        setupVmStateDirectory();
    }
};

TEST_F(MemoryTimingTest, ZeroLatenciesChangeNothing)
{
    const std::string source = loadThenUse(1);
    EXPECT_EQ(extraCycles<RV5StageProcessorHF>(source, uniform(0, false)), 0);
    EXPECT_EQ(extraCycles<RV5StageProcessorHF>(source, uniform(0, true)), 0);
    EXPECT_EQ(extraCycles<RV5StageProcessorNHNF>(loadThenUse(4), uniform(0, false)), 0);
}

TEST_F(MemoryTimingTest, ABlockingLoadCostsItsFullLatencyWhereverItsConsumerIs)
{
    for (int distance : {1, 4})
    {
        EXPECT_EQ(extraCycles<RV5StageProcessorHF>(loadThenUse(distance), uniform(5, false)), 5);
        EXPECT_EQ(extraCycles<RV5StageProcessorHNF>(loadThenUse(distance), uniform(5, false)), 5);
    }
    // The no-hazard variants need the consumer far enough away to be correct at all.
    EXPECT_EQ(extraCycles<RV5StageProcessorNHF>(loadThenUse(4), uniform(5, false)), 5);
    EXPECT_EQ(extraCycles<RV5StageProcessorNHNF>(loadThenUse(4), uniform(5, false)), 5);
}

TEST_F(MemoryTimingTest, ANonBlockingLoadOnlyDelaysItsConsumerWithForwarding)
{
    // The consumer must be 2 + extra slots after the load; anything closer waits the difference.
    const int extra = 3;
    for (int distance = 1; distance <= 6; ++distance)
    {
        const int expected = std::max(0, 2 + extra - distance) - std::max(0, 2 - distance);
        EXPECT_EQ(extraCycles<RV5StageProcessorHF>(loadThenUse(distance), uniform(extra, true)),
                  expected)
            << "distance " << distance;
    }
}

TEST_F(MemoryTimingTest, ANonBlockingLoadOnlyDelaysItsConsumerWithoutForwarding)
{
    // Without forwarding the consumer must be 3 + extra slots after the load.
    const int extra = 3;
    for (int distance = 1; distance <= 7; ++distance)
    {
        const int expected = std::max(0, 3 + extra - distance) - std::max(0, 3 - distance);
        EXPECT_EQ(extraCycles<RV5StageProcessorHNF>(loadThenUse(distance), uniform(extra, true)),
                  expected)
            << "distance " << distance;
    }
}

TEST_F(MemoryTimingTest, ANonBlockingLoadNobodyReadsCostsNothing)
{
    const std::string source = loadThenUse(1, /*consumerReadsLoad=*/false);
    EXPECT_EQ(extraCycles<RV5StageProcessorHF>(source, uniform(8, true)), 0);
    EXPECT_EQ(extraCycles<RV5StageProcessorHNF>(source, uniform(8, true)), 0);
}

TEST_F(MemoryTimingTest, VariantsWithoutHazardDetectionAlwaysBlock)
{
    // Nothing could make a consumer wait for a late load, so the pipeline waits instead.
    EXPECT_EQ(extraCycles<RV5StageProcessorNHF>(loadThenUse(4), uniform(5, true)), 5);
    EXPECT_EQ(extraCycles<RV5StageProcessorNHNF>(loadThenUse(4), uniform(5, true)), 5);
}

TEST_F(MemoryTimingTest, ResultsDoNotChange)
{
    const std::string source = loadThenUse(1);
    const AssembledProgram program = assembleSource("lui x10, 2\n addi x7, x0, 42\n" + nops(3) +
                                                    "sw x7, 0(x10)\n" + nops(3) + source);
    const Snapshot plain = runOn<RV5StageProcessorHF>(program).snapshot;
    const Snapshot slow =
        runOn<RV5StageProcessorHF>(program, [](RV5StageProcessorHF &vm)
                                   { vm.memory_timing_ = uniform(7, true); })
            .snapshot;
    EXPECT_TRUE(plain == slow);
    EXPECT_EQ(slow.gprs[6], 84u);
}

TEST_F(MemoryTimingTest, TheLatencyDependsOnWhichLevelServesTheLoad)
{
    // L1 holds two 16-byte lines; L2 is large enough to keep everything.
    const auto configure = [](RV5StageProcessorHF &vm, const MemoryTiming &timing)
    {
        vm.memory_controller_.getL2Cache()->reconfigure(
            CacheConfig{8, 16, 4, WritePolicy::WriteBack, AllocationPolicy::WriteAllocate,
                        ReplacementPolicy::LRU});
        vm.memory_controller_.getL1Cache()->reconfigure(
            CacheConfig{1, 16, 2, WritePolicy::WriteBack, AllocationPolicy::WriteAllocate,
                        ReplacementPolicy::LRU});
        vm.memory_timing_ = timing;
    };

    // A from memory, A again from L1, B and C from memory (evicting A from L1), A from L2.
    const std::string source = "lui x10, 2\n" + nops(4) +
                               "lw x5, 0(x10)\n" + nops(2) + "lw x6, 0(x10)\n" + nops(2) +
                               "lw x7, 16(x10)\n" + nops(2) + "lw x8, 32(x10)\n" + nops(2) +
                               "lw x9, 0(x10)\n" + nops(4);
    MemoryTiming timing;
    timing.enabled = true;
    timing.l1HitCycles = 1;
    timing.l2HitCycles = 4;
    timing.memoryCycles = 20;

    const AssembledProgram program = assembleSource(source);
    const unsigned off = runOn<RV5StageProcessorHF>(
                             program, [&](RV5StageProcessorHF &vm) { configure(vm, MemoryTiming{}); })
                             .cycles;
    const unsigned on =
        runOn<RV5StageProcessorHF>(program, [&](RV5StageProcessorHF &vm) { configure(vm, timing); })
            .cycles;
    EXPECT_EQ(on - off, 20u + 1u + 20u + 20u + 4u);
}

namespace
{
// Runs to completion, but on the way steps forward, undoes some steps and continues. With the
// latency state saved in every step, the result must be the same as a straight run.
template <typename Vm> unsigned cyclesWithUndoDetour(const std::string &source,
                                                    const MemoryTiming &timing, int forward,
                                                    int back)
{
    QuietOutput quiet;
    Vm vm;
    vm.memory_timing_ = timing;
    vm.LoadProgram(assembleSource(source));
    for (int i = 0; i < forward; ++i)
    {
        vm.Step();
    }
    for (int i = 0; i < back; ++i)
    {
        vm.Undo();
    }
    vm.DebugRun();
    return vm.cycle_s_;
}
} // namespace

TEST_F(MemoryTimingTest, UndoRestoresTheLatencyState)
{
    const std::string source = loadThenUse(1) + loadThenUse(2);
    for (bool nonBlocking : {false, true})
    {
        const MemoryTiming timing = uniform(4, nonBlocking);
        const unsigned straight = cycles<RV5StageProcessorHF>(source, timing);
        for (int forward : {8, 10, 12})
        {
            EXPECT_EQ(cyclesWithUndoDetour<RV5StageProcessorHF>(source, timing, forward, 3),
                      straight)
                << "nonBlocking " << nonBlocking << ", forward " << forward;
        }
    }
}

TEST_F(MemoryTimingTest, ANewerWriteCancelsTheWaitForALoad)
{
    // addi overwrites x5 while x5's load is still on its way. Nothing reads the loaded value any
    // more, so nothing waits for it, and the add sees the addi's 1.
    const std::string source = "lui x10, 2\n" + nops(4) +
                               "lw x5, 0(x10)\naddi x5, x0, 1\nadd x6, x5, x5\n" + nops(4);
    const RunResult slow = runOn<RV5StageProcessorHF>(
        assembleSource(source), [](RV5StageProcessorHF &vm) { vm.memory_timing_ = uniform(4, true); });
    EXPECT_EQ(slow.snapshot.gprs[6], 2u);
    EXPECT_EQ(extraCycles<RV5StageProcessorHF>(source, uniform(4, true)), 0);
    EXPECT_EQ(extraCycles<RV5StageProcessorHNF>(source, uniform(4, true)), 0);
}
