// Helpers shared by the optimizer tests: a random straight-line program generator, a way to run an
// assembled program on any processor model and capture its architectural state, and utilities to
// build a program with its units reordered.
#pragma once

#include <array>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "optimizer/dependency_graph.h"
#include "optimizer/timeline.h"
#include "processor/rvss/rvss_processor.h"

namespace optimizer_test
{
// Memory scratch area. The two base registers point here, sometimes at the same address and
// sometimes at overlapping ones, so loads and stores through different bases really do alias.
inline constexpr uint64_t kScratchBase = 8192;

struct Snapshot
{
    std::array<uint64_t, 32> gprs{};
    std::array<uint64_t, 32> fprs{};
    std::vector<uint32_t> memory;

    bool operator==(const Snapshot &other) const = default;
};

struct RunResult
{
    Snapshot snapshot;
    unsigned cycles{0};
};

// Silences the per-instruction output of DebugRun for the guard's lifetime.
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

// Runs a program to completion on a fresh processor of type Vm.
template <typename Vm> RunResult runOn(const Kites::AssembledProgram &program)
{
    QuietOutput quiet;
    Vm vm;
    vm.LoadProgram(program);
    vm.DebugRun();

    RunResult result;
    for (size_t i = 0; i < 32; ++i)
    {
        result.snapshot.gprs[i] = vm.registers_.ReadGpr(i);
        result.snapshot.fprs[i] = vm.registers_.ReadFpr(i);
    }
    // A window around the scratch area, wide enough to cover the base registers moving.
    for (uint64_t address = kScratchBase - 16; address < kScratchBase + 128; address += 4)
    {
        result.snapshot.memory.push_back(vm.memory_controller_.readWord(address));
    }
    result.cycles = vm.cycle_s_;
    return result;
}

inline Kites::AssembledProgram assembleSource(const std::string &source)
{
    std::istringstream stream(source);
    return Kites::assemble(stream);
}

// The program with its units placed in `order` (order[k] is the block-local unit at slot k).
inline Kites::AssembledProgram withOrder(const Kites::AssembledProgram &program,
                                         const Kites::optimizer::ProgramModel &model,
                                         const Kites::optimizer::BasicBlock &block,
                                         const std::vector<size_t> &order)
{
    Kites::AssembledProgram reordered = program;
    reordered.text_buffer.clear();
    for (size_t node : order)
    {
        const Kites::optimizer::Unit &unit = model.units[block.firstUnit + node];
        for (size_t k = unit.firstInstruction; k < unit.firstInstruction + unit.instructionCount;
             ++k)
        {
            reordered.text_buffer.push_back(program.text_buffer[k]);
        }
    }
    return reordered;
}

// The program with the timeline's bubbles inserted as real NOP instructions, one position per word.
// Only valid for straight-line code, where inserting words cannot disturb a branch offset.
inline Kites::AssembledProgram paddedWithNops(const Kites::AssembledProgram &program,
                                              const Kites::optimizer::Timeline &timeline)
{
    constexpr uint32_t kNopWord = 0x00000013; // addi x0, x0, 0
    Kites::AssembledProgram result = program;
    result.text_buffer.clear();
    for (size_t position = 0; position < program.text_buffer.size(); ++position)
    {
        for (int bubble = 0; bubble < timeline.bubblesBefore[position]; ++bubble)
        {
            result.text_buffer.push_back(kNopWord);
        }
        result.text_buffer.push_back(program.text_buffer[position]);
    }
    return result;
}

// A uniformly random choice among the units that are ready at each step.
inline std::vector<size_t> randomLegalOrder(const Kites::optimizer::DependencyGraph &graph,
                                            std::mt19937 &rng)
{
    std::vector<size_t> remainingPredecessors(graph.nodeCount, 0);
    for (const Kites::optimizer::Dependency &edge : graph.edges)
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
//
// With includeFloatingPoint false only integer instructions are generated. The pipelined
// processor models only support the I and M extensions, so that is what their tests use.
inline std::string randomProgram(std::mt19937 &rng, size_t bodyLength = 16,
                                 bool includeFloatingPoint = true)
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
    if (includeFloatingPoint)
    {
        out << "fcvt.s.w f1, x5\nfcvt.s.w f2, x6\n";
    }

    for (size_t i = 0; i < bodyLength; ++i)
    {
        switch (pick(0, includeFloatingPoint ? 14 : 11))
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

// How many random programs a test should use: `defaultCount`, unless the environment variable
// named `variable` holds a larger positive number (for longer soak runs).
inline size_t programCountFromEnvironment(const char *variable, size_t defaultCount)
{
    if (const char *value = std::getenv(variable))
    {
        const long parsed = std::strtol(value, nullptr, 10);
        if (parsed > 0)
        {
            return static_cast<size_t>(parsed);
        }
    }
    return defaultCount;
}
} // namespace optimizer_test
