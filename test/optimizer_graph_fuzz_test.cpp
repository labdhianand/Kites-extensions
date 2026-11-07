// Soundness check for the dependency graph.
//
// The graph claims that any order respecting its edges computes the same result as the original
// program. This test checks that claim on the simulator: it generates random straight-line
// programs, draws random legal orders from the graph, runs every order on the single-cycle
// processor (which has no hazards, so the result depends only on the order's meaning) and requires
// identical registers and memory.

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "optimizer/dependency_graph.h"
#include "optimizer_test_support.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

using namespace Kites;
using namespace Kites::optimizer;
using namespace optimizer_test;

namespace
{
// Each run builds a fresh processor (about a quarter of a second), so the default is kept small.
// Set KITES_FUZZ_PROGRAMS for a longer soak run, for example KITES_FUZZ_PROGRAMS=200.
constexpr size_t kDefaultProgramCount = 15;
constexpr size_t kOrdersPerProgram = 3;

Snapshot runOnSingleCycle(const AssembledProgram &program)
{
    return runOn<RVSSProcessor>(program).snapshot;
}
} // namespace

TEST(OptimizerGraphFuzz, EveryLegalOrderGivesTheSameResult)
{
    setupVmStateDirectory();
    std::mt19937 rng(20261107); // fixed seed, so a failure is reproducible

    const size_t programCount =
        programCountFromEnvironment("KITES_FUZZ_PROGRAMS", kDefaultProgramCount);
    size_t reorderedRunCount = 0;
    size_t programsWithFreedomCount = 0;

    for (size_t programIndex = 0; programIndex < programCount; ++programIndex)
    {
        const std::string source = randomProgram(rng);
        SCOPED_TRACE("program " + std::to_string(programIndex) + ":\n" + source);

        const AssembledProgram program = assembleSource(source);
        const ProgramModel model = buildProgramModel(program);
        ASSERT_EQ(model.blocks.size(), 1u) << "generator must produce a single basic block";

        const BasicBlock &block = model.blocks[0];
        const DependencyGraph graph = buildDependencyGraph(model, block);
        const Snapshot expected = runOnSingleCycle(program);

        std::vector<size_t> identity(graph.nodeCount);
        for (size_t i = 0; i < identity.size(); ++i)
        {
            identity[i] = i;
        }

        bool foundDifferentOrder = false;
        for (size_t attempt = 0; attempt < kOrdersPerProgram; ++attempt)
        {
            const std::vector<size_t> order = randomLegalOrder(graph, rng);
            ASSERT_TRUE(graph.isRespectedBy(order));

            const Snapshot actual = runOnSingleCycle(withOrder(program, model, block, order));
            EXPECT_TRUE(actual == expected) << "a legal order changed the program's result";

            if (order != identity)
            {
                ++reorderedRunCount;
                foundDifferentOrder = true;
            }
        }
        if (foundDifferentOrder)
        {
            ++programsWithFreedomCount;
        }
    }

    // The check is only meaningful if the graph leaves real freedom to reorder.
    EXPECT_GT(programsWithFreedomCount, programCount / 2);
    EXPECT_GT(reorderedRunCount, programCount);
}

// Negative control: the harness must be able to tell a wrong order from a right one. Here the
// load reads a value the store just wrote, so swapping them changes the result.
TEST(OptimizerGraphFuzz, HarnessDetectsAnIllegalOrder)
{
    setupVmStateDirectory();
    const AssembledProgram program = assembleSource(R"(
        li x5, 7
        li x10, 8192
        sw x5, 0(x10)
        lw x6, 0(x10)
    )");
    const ProgramModel model = buildProgramModel(program);
    const BasicBlock &block = model.blocks.at(0);
    const DependencyGraph graph = buildDependencyGraph(model, block);

    const std::vector<size_t> original = {0, 1, 2, 3};
    const std::vector<size_t> illegal = {0, 1, 3, 2}; // load before the store it depends on

    EXPECT_TRUE(graph.isRespectedBy(original));
    EXPECT_FALSE(graph.isRespectedBy(illegal));

    const Snapshot good = runOnSingleCycle(withOrder(program, model, block, original));
    const Snapshot bad = runOnSingleCycle(withOrder(program, model, block, illegal));
    EXPECT_EQ(good.gprs[6], 7u);
    EXPECT_FALSE(good == bad);
}
