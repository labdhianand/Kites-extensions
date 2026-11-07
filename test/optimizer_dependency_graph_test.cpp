#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "assembler/assembler.h"
#include "optimizer/dependency_graph.h"

using namespace Kites;
using namespace Kites::optimizer;

namespace
{
struct Analysis
{
    ProgramModel model;
    DependencyGraph graph; // graph of the first basic block
};

Analysis analyse(const std::string &source)
{
    std::istringstream stream(source);
    Analysis analysis;
    analysis.model = buildProgramModel(assemble(stream));
    analysis.graph = buildDependencyGraph(analysis.model, analysis.model.blocks.at(0));
    return analysis;
}

bool hasEdge(const DependencyGraph &graph, size_t from, size_t to, DependencyKind kind)
{
    for (const Dependency &edge : graph.edges)
    {
        if (edge.from == from && edge.to == to && edge.kind == kind)
        {
            return true;
        }
    }
    return false;
}

// Any edge at all between the two nodes, in either direction and of any kind.
bool hasAnyEdge(const DependencyGraph &graph, size_t a, size_t b)
{
    for (const Dependency &edge : graph.edges)
    {
        if ((edge.from == a && edge.to == b) || (edge.from == b && edge.to == a))
        {
            return true;
        }
    }
    return false;
}
} // namespace

// ---------------------------------------------------------------------------
// Register dependencies
// ---------------------------------------------------------------------------
TEST(OptimizerDependencyGraph, ReadAfterWriteIsADependency)
{
    const Analysis a = analyse("add x3, x1, x2\n add x4, x3, x2\n");
    EXPECT_TRUE(hasEdge(a.graph, 0, 1, DependencyKind::RegisterRaw));
}

TEST(OptimizerDependencyGraph, IndependentInstructionsHaveNoEdge)
{
    const Analysis a = analyse("add x3, x1, x2\n add x5, x6, x7\n");
    EXPECT_TRUE(a.graph.edges.empty());
}

TEST(OptimizerDependencyGraph, WriteAfterReadIsADependency)
{
    const Analysis a = analyse("add x3, x1, x2\n add x1, x5, x6\n");
    EXPECT_TRUE(hasEdge(a.graph, 0, 1, DependencyKind::RegisterWar));
    EXPECT_FALSE(hasEdge(a.graph, 0, 1, DependencyKind::RegisterRaw));
}

TEST(OptimizerDependencyGraph, WriteAfterWriteIsADependency)
{
    const Analysis a = analyse("add x3, x1, x2\n add x3, x5, x6\n");
    EXPECT_TRUE(hasEdge(a.graph, 0, 1, DependencyKind::RegisterWaw));
}

TEST(OptimizerDependencyGraph, ZeroRegisterNeverCreatesADependency)
{
    const Analysis a = analyse("add x0, x1, x2\n add x3, x0, x4\n");
    EXPECT_FALSE(hasAnyEdge(a.graph, 0, 1));
}

TEST(OptimizerDependencyGraph, IntegerAndFloatRegistersWithSameNumberAreIndependent)
{
    const Analysis a = analyse("fadd.s f5, f1, f2\n add x6, x5, x5\n");
    EXPECT_FALSE(hasAnyEdge(a.graph, 0, 1));
}

TEST(OptimizerDependencyGraph, OnlyTheLatestWriterIsARawSource)
{
    const Analysis a = analyse("add x3, x1, x2\n add x3, x4, x5\n add x6, x3, x3\n");
    EXPECT_TRUE(hasEdge(a.graph, 1, 2, DependencyKind::RegisterRaw));
    EXPECT_FALSE(hasEdge(a.graph, 0, 2, DependencyKind::RegisterRaw));
}

TEST(OptimizerDependencyGraph, RegisterTheUnitWroteItselfIsNotAnOutsideRead)
{
    // li t0, 100000 expands to lui + addi on the same register. The addi's read of t0 comes from
    // the lui, so it must not make the unit depend on the earlier writer of t0.
    const Analysis a = analyse("add t0, x1, x2\n li t0, 100000\n");
    EXPECT_TRUE(hasEdge(a.graph, 0, 1, DependencyKind::RegisterWaw));
    EXPECT_FALSE(hasEdge(a.graph, 0, 1, DependencyKind::RegisterRaw));
}

// ---------------------------------------------------------------------------
// Memory dependencies
// ---------------------------------------------------------------------------
TEST(OptimizerDependencyGraph, LoadAfterStoreToSameAddressIsADependency)
{
    const Analysis a = analyse("sw x1, 0(x2)\n lw x3, 0(x2)\n");
    EXPECT_TRUE(hasEdge(a.graph, 0, 1, DependencyKind::MemoryRaw));
}

TEST(OptimizerDependencyGraph, StoreAfterLoadAndStoreAfterStoreAreDependencies)
{
    const Analysis war = analyse("lw x3, 0(x2)\n sw x1, 0(x2)\n");
    EXPECT_TRUE(hasEdge(war.graph, 0, 1, DependencyKind::MemoryWar));

    const Analysis waw = analyse("sw x3, 0(x2)\n sw x1, 0(x2)\n");
    EXPECT_TRUE(hasEdge(waw.graph, 0, 1, DependencyKind::MemoryWaw));
}

TEST(OptimizerDependencyGraph, TwoLoadsAreIndependent)
{
    const Analysis a = analyse("lw x1, 0(x2)\n lw x3, 0(x2)\n");
    EXPECT_FALSE(hasAnyEdge(a.graph, 0, 1));
}

TEST(OptimizerDependencyGraph, NonOverlappingOffsetsFromSameBaseAreIndependent)
{
    const Analysis a = analyse("sw x1, 0(x2)\n lw x3, 8(x2)\n");
    EXPECT_FALSE(hasAnyEdge(a.graph, 0, 1));
}

TEST(OptimizerDependencyGraph, OverlappingByteRangesAreDependent)
{
    // sw covers bytes 0-3; lb at offset 3 touches byte 3, lb at offset 4 does not.
    const Analysis overlap = analyse("sw x1, 0(x2)\n lb x3, 3(x2)\n");
    EXPECT_TRUE(hasEdge(overlap.graph, 0, 1, DependencyKind::MemoryRaw));

    const Analysis adjacent = analyse("sw x1, 0(x2)\n lb x3, 4(x2)\n");
    EXPECT_FALSE(hasAnyEdge(adjacent.graph, 0, 1));
}

TEST(OptimizerDependencyGraph, DifferentBaseRegistersAreAssumedToAlias)
{
    const Analysis a = analyse("sw x1, 0(x2)\n lw x3, 0(x4)\n");
    EXPECT_TRUE(hasEdge(a.graph, 0, 1, DependencyKind::MemoryRaw));
}

TEST(OptimizerDependencyGraph, ChangedBaseRegisterDefeatsTheDisjointnessProof)
{
    // Same register and offsets that look different, but x2 changes in between.
    const Analysis a = analyse("sw x1, 0(x2)\n addi x2, x2, 8\n lw x3, 0(x2)\n");
    EXPECT_TRUE(hasEdge(a.graph, 0, 2, DependencyKind::MemoryRaw));
}

TEST(OptimizerDependencyGraph, AbsoluteAddressesThroughZeroRegisterCanBeCompared)
{
    const Analysis different = analyse("sw x1, 256(x0)\n lw x3, 512(x0)\n");
    EXPECT_FALSE(hasAnyEdge(different.graph, 0, 1));

    const Analysis same = analyse("sw x1, 256(x0)\n lw x3, 256(x0)\n");
    EXPECT_TRUE(hasEdge(same.graph, 0, 1, DependencyKind::MemoryRaw));
}

TEST(OptimizerDependencyGraph, FloatingPointLoadsAndStoresFollowTheSameRules)
{
    const Analysis a = analyse("fsw f1, 0(x2)\n flw f3, 0(x2)\n");
    EXPECT_TRUE(hasEdge(a.graph, 0, 1, DependencyKind::MemoryRaw));
}

// ---------------------------------------------------------------------------
// Fences and the terminator
// ---------------------------------------------------------------------------
TEST(OptimizerDependencyGraph, UnitThatReadsItsOwnAddressKeepsItsPosition)
{
    const Analysis a = analyse(R"(
    .data
    v: .word 1
    .text
        add x3, x4, x5
        la  x6, v
        add x7, x8, x9
    )");
    ASSERT_EQ(a.graph.nodeCount, 3u);
    EXPECT_TRUE(hasEdge(a.graph, 0, 1, DependencyKind::Fence));
    EXPECT_TRUE(hasEdge(a.graph, 1, 2, DependencyKind::Fence));

    EXPECT_TRUE(a.graph.isRespectedBy({0, 1, 2}));
    EXPECT_FALSE(a.graph.isRespectedBy({1, 0, 2}));
    EXPECT_FALSE(a.graph.isRespectedBy({0, 2, 1}));
}

TEST(OptimizerDependencyGraph, EcallIsAFenceAndStaysLast)
{
    const Analysis a = analyse("add x3, x4, x5\n add x6, x7, x8\n addi a7, x0, 10\n ecall\n");
    const size_t last = a.graph.nodeCount - 1;
    for (size_t node = 0; node < last; ++node)
    {
        EXPECT_TRUE(hasEdge(a.graph, node, last, DependencyKind::Fence)) << node;
    }
}

TEST(OptimizerDependencyGraph, BranchStaysLastInItsBlock)
{
    const Analysis a = analyse(R"(
        add x3, x4, x5
        add x6, x7, x8
        beq x1, x2, done
    done:
        add x9, x9, x9
    )");
    ASSERT_EQ(a.graph.nodeCount, 3u);
    EXPECT_TRUE(hasEdge(a.graph, 0, 2, DependencyKind::Control));
    EXPECT_TRUE(hasEdge(a.graph, 1, 2, DependencyKind::Control));

    // The two adds are independent of each other, so they may swap, but not pass the branch.
    EXPECT_TRUE(a.graph.isRespectedBy({1, 0, 2}));
    EXPECT_FALSE(a.graph.isRespectedBy({0, 2, 1}));
}

// ---------------------------------------------------------------------------
// isRespectedBy
// ---------------------------------------------------------------------------
TEST(OptimizerDependencyGraph, IsRespectedByRejectsMalformedOrders)
{
    const Analysis a = analyse("add x3, x1, x2\n add x5, x6, x7\n add x8, x9, x10\n");
    EXPECT_TRUE(a.graph.isRespectedBy({2, 0, 1})); // everything is independent
    EXPECT_FALSE(a.graph.isRespectedBy({0, 1}));        // too short
    EXPECT_FALSE(a.graph.isRespectedBy({0, 1, 1}));     // repeats a node
    EXPECT_FALSE(a.graph.isRespectedBy({0, 1, 5}));     // node out of range
    EXPECT_FALSE(a.graph.isRespectedBy({0, 1, 2, 0}));  // too long
}

TEST(OptimizerDependencyGraph, ReadAfterWriteBlocksSwapping)
{
    const Analysis a = analyse("add x3, x1, x2\n add x4, x3, x2\n");
    EXPECT_TRUE(a.graph.isRespectedBy({0, 1}));
    EXPECT_FALSE(a.graph.isRespectedBy({1, 0}));
}

// ---------------------------------------------------------------------------
// Structural invariants for every block of every example program.
// ---------------------------------------------------------------------------
TEST(OptimizerDependencyGraphInvariants, HoldForAllExamplePrograms)
{
    size_t blockCount = 0;
    size_t edgeCount = 0;

    for (const char *directory : {"../examples", "../examples/fd-extension"})
    {
        if (!std::filesystem::exists(directory))
        {
            continue;
        }
        for (const auto &entry : std::filesystem::directory_iterator(directory))
        {
            if (entry.path().extension() != ".s")
            {
                continue;
            }
            std::ifstream file(entry.path());
            const ProgramModel model = buildProgramModel(assemble(file));

            for (const BasicBlock &block : model.blocks)
            {
                const DependencyGraph graph = buildDependencyGraph(model, block);
                EXPECT_EQ(graph.nodeCount, block.unitCount) << entry.path().filename();

                // Edges always point forward, so the original order is always legal (and the
                // graph has no cycles).
                for (const Dependency &edge : graph.edges)
                {
                    EXPECT_LT(edge.from, edge.to) << entry.path().filename();
                    EXPECT_LT(edge.to, graph.nodeCount) << entry.path().filename();
                }
                std::vector<size_t> original(graph.nodeCount);
                for (size_t i = 0; i < original.size(); ++i)
                {
                    original[i] = i;
                }
                EXPECT_TRUE(graph.isRespectedBy(original)) << entry.path().filename();

                ++blockCount;
                edgeCount += graph.edges.size();
            }
        }
    }

    EXPECT_GT(blockCount, 20u);
    EXPECT_GT(edgeCount, 50u);
}
