// Soundness check for the dependency graph.
//
// The graph claims that any order respecting its edges computes the same result as the original
// program. This test checks that claim on the simulator: it generates random straight-line
// programs, draws random legal orders from the graph, runs every order on the single-cycle
// processor (which has no hazards, so the result depends only on the order's meaning) and requires
// identical registers and memory.

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "optimizer/dependency_graph.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

using namespace Kites;
using namespace Kites::optimizer;

namespace
{
// Each run builds a fresh processor (about a quarter of a second), so the default is kept small.
// Set KITES_FUZZ_PROGRAMS for a longer soak run, for example KITES_FUZZ_PROGRAMS=200.
constexpr size_t kDefaultProgramCount = 15;
constexpr size_t kOrdersPerProgram = 3;
constexpr size_t kBodyLength = 16;

size_t programCountFromEnvironment()
{
    if (const char *value = std::getenv("KITES_FUZZ_PROGRAMS"))
    {
        const long parsed = std::strtol(value, nullptr, 10);
        if (parsed > 0)
        {
            return static_cast<size_t>(parsed);
        }
    }
    return kDefaultProgramCount;
}

// Memory scratch area. The two base registers point here, sometimes at the same address and
// sometimes at overlapping ones, so loads and stores through different bases really do alias.
constexpr uint64_t kScratchBase = 8192;

struct Snapshot
{
    std::array<uint64_t, 32> gprs{};
    std::array<uint64_t, 32> fprs{};
    std::vector<uint32_t> memory;

    bool operator==(const Snapshot &other) const = default;
};

// Silences the per-instruction "Program Counter" output of DebugRun for the guard's lifetime.
class QuietOutput
{
  public:
    QuietOutput() : m_previous(std::cout.rdbuf(m_sink.rdbuf()))
    {
    }
    ~QuietOutput()
    {
        std::cout.rdbuf(m_previous);
    }

  private:
    std::ostringstream m_sink;
    std::streambuf *m_previous;
};

Snapshot runOnSingleCycle(const AssembledProgram &program)
{
    QuietOutput quiet;
    RVSSProcessor vm;
    vm.LoadProgram(program);
    vm.DebugRun();

    Snapshot snapshot;
    for (size_t i = 0; i < 32; ++i)
    {
        snapshot.gprs[i] = vm.registers_.ReadGpr(i);
        snapshot.fprs[i] = vm.registers_.ReadFpr(i);
    }
    // A window around the scratch area, wide enough to cover the base registers moving.
    for (uint64_t address = kScratchBase - 16; address < kScratchBase + 128; address += 4)
    {
        snapshot.memory.push_back(vm.memory_controller_.readWord(address));
    }
    return snapshot;
}

// The program with its units placed in `order` (order[k] is the block-local unit at slot k).
AssembledProgram withOrder(const AssembledProgram &program, const ProgramModel &model,
                           const BasicBlock &block, const std::vector<size_t> &order)
{
    AssembledProgram reordered = program;
    reordered.text_buffer.clear();
    for (size_t node : order)
    {
        const Unit &unit = model.units[block.firstUnit + node];
        for (size_t k = unit.firstInstruction; k < unit.firstInstruction + unit.instructionCount;
             ++k)
        {
            reordered.text_buffer.push_back(program.text_buffer[k]);
        }
    }
    return reordered;
}

// A uniformly random choice among the units that are ready at each step.
std::vector<size_t> randomLegalOrder(const DependencyGraph &graph, std::mt19937 &rng)
{
    std::vector<size_t> remainingPredecessors(graph.nodeCount, 0);
    for (const Dependency &edge : graph.edges)
    {
        ++remainingPredecessors[edge.to];
    }

    std::vector<size_t> ready;
    for (size_t node = 0; node < graph.nodeCount; ++node)
    {
        if (remainingPredecessors[node] == 0)
        {
            ready.push_back(node);
        }
    }

    std::vector<size_t> order;
    while (!ready.empty())
    {
        const size_t pick = std::uniform_int_distribution<size_t>(0, ready.size() - 1)(rng);
        const size_t node = ready[pick];
        ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(pick));
        order.push_back(node);

        for (size_t edgeIndex : graph.outgoing[node])
        {
            const size_t successor = graph.edges[edgeIndex].to;
            if (--remainingPredecessors[successor] == 0)
            {
                ready.push_back(successor);
            }
        }
    }
    return order;
}

// A random straight-line program (a single basic block) mixing integer and floating-point
// arithmetic, loads and stores through two possibly-aliasing base registers, a base register that
// moves mid-program, and multi-instruction `li` expansions.
std::string randomProgram(std::mt19937 &rng)
{
    auto pick = [&](size_t lowest, size_t highest)
    { return std::uniform_int_distribution<size_t>(lowest, highest)(rng); };

    auto gprName = [&]() { return "x" + std::to_string(pick(5, 9)); };
    auto fprName = [&]() { return "f" + std::to_string(pick(1, 4)); };
    auto baseName = [&]() { return pick(0, 1) == 0 ? std::string("x10") : std::string("x11"); };
    auto wordOffset = [&]() { return std::to_string(pick(0, 3) * 4); };

    std::ostringstream out;
    out << "li x5, 11\nli x6, 22\nli x7, 33\nli x8, 44\nli x9, 55\n";
    out << "li x10, " << kScratchBase << "\n";
    const char *secondBase[] = {"8192", "8196", "8200"};
    out << "li x11, " << secondBase[pick(0, 2)] << "\n";
    out << "fcvt.s.w f1, x5\nfcvt.s.w f2, x6\n";

    for (size_t i = 0; i < kBodyLength; ++i)
    {
        switch (pick(0, 14))
        {
        case 0:
            out << "add " << gprName() << ", " << gprName() << ", " << gprName() << "\n";
            break;
        case 1:
            out << "sub " << gprName() << ", " << gprName() << ", " << gprName() << "\n";
            break;
        case 2:
            out << "xor " << gprName() << ", " << gprName() << ", " << gprName() << "\n";
            break;
        case 3:
            out << "addi " << gprName() << ", " << gprName() << ", "
                << static_cast<int>(pick(0, 16)) - 8 << "\n";
            break;
        case 4:
        case 5:
            out << "sw " << gprName() << ", " << wordOffset() << "(" << baseName() << ")\n";
            break;
        case 6:
        case 7:
            out << "lw " << gprName() << ", " << wordOffset() << "(" << baseName() << ")\n";
            break;
        case 8:
            out << "sb " << gprName() << ", " << pick(0, 7) << "(" << baseName() << ")\n";
            break;
        case 9:
            out << "lbu " << gprName() << ", " << pick(0, 7) << "(" << baseName() << ")\n";
            break;
        case 10:
            out << "addi " << baseName() << ", " << baseName() << ", 4\n";
            break;
        case 11:
            out << "li " << gprName() << ", " << 100000 + pick(0, 3) << "\n";
            break;
        case 12:
            out << "fadd.s " << fprName() << ", " << fprName() << ", " << fprName() << "\n";
            break;
        case 13:
            if (pick(0, 1) == 0)
            {
                out << "fsw " << fprName() << ", " << wordOffset() << "(" << baseName() << ")\n";
            }
            else
            {
                out << "flw " << fprName() << ", " << wordOffset() << "(" << baseName() << ")\n";
            }
            break;
        default:
            if (pick(0, 1) == 0)
            {
                out << "fcvt.w.s " << gprName() << ", " << fprName() << "\n";
            }
            else
            {
                out << "fcvt.s.w " << fprName() << ", " << gprName() << "\n";
            }
            break;
        }
    }
    return out.str();
}
} // namespace

TEST(OptimizerGraphFuzz, EveryLegalOrderGivesTheSameResult)
{
    setupVmStateDirectory();
    std::mt19937 rng(20261107); // fixed seed, so a failure is reproducible

    const size_t programCount = programCountFromEnvironment();
    size_t reorderedRunCount = 0;
    size_t programsWithFreedomCount = 0;

    for (size_t programIndex = 0; programIndex < programCount; ++programIndex)
    {
        const std::string source = randomProgram(rng);
        SCOPED_TRACE("program " + std::to_string(programIndex) + ":\n" + source);

        std::istringstream stream(source);
        const AssembledProgram program = assemble(stream);
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
    std::istringstream stream(R"(
        li x5, 7
        li x10, 8192
        sw x5, 0(x10)
        lw x6, 0(x10)
    )");
    const AssembledProgram program = assemble(stream);
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
