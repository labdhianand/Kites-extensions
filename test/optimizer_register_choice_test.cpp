// Tests for which spare registers loop unrolling hands out: temporaries before argument registers,
// and the callee-saved s-registers only when nothing else is free.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "optimizer/loop_unroller.h"

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

UnrollResult unrollBy2(const std::string &source)
{
    UnrollOptions options;
    options.maxFactor = 2;
    return unrollLoops(source, options);
}
} // namespace

TEST(OptimizerRegisterChoice, CalleeSavedRegistersAreTheSRegisters)
{
    for (unsigned reg : {8u, 9u, 18u, 22u, 27u})
    {
        EXPECT_TRUE(isCalleeSaved(reg)) << "x" << reg;
    }
    for (unsigned reg : {0u, 1u, 2u, 5u, 7u, 10u, 17u, 28u, 31u})
    {
        EXPECT_FALSE(isCalleeSaved(reg)) << "x" << reg;
    }
}

TEST(OptimizerRegisterChoice, VectorAddUnrollsWithTemporariesOnly)
{
    const std::string source = readFile("../benchmarks/vector_add.s");
    ASSERT_FALSE(source.empty());
    const UnrollResult result = unrollBy2(source);
    ASSERT_EQ(result.loops.size(), 1u);
    ASSERT_TRUE(result.loops[0].unrolled);
    ASSERT_FALSE(result.loops[0].renamedTo.empty());
    for (unsigned reg : result.loops[0].renamedTo)
    {
        EXPECT_FALSE(isCalleeSaved(reg)) << "x" << reg << " is callee-saved";
    }
}

TEST(OptimizerRegisterChoice, FallsBackToCalleeSavedWhenTemporariesAreTaken)
{
    // Every temporary and argument register is used, so the copy of the loop body has to borrow
    // s-registers - and must say so through renamedTo.
    std::string source = ".text\n";
    for (unsigned reg : {5u, 6u, 7u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 28u, 29u, 30u, 31u})
    {
        source += "    addi x" + std::to_string(reg) + ", x0, 1\n";
    }
    source += "    addi x9, x0, 0\n"
              "    addi x8, x0, 4\n"
              "loop:\n"
              "    add  x18, x9, x9\n"
              "    add  x19, x18, x5\n"
              "    addi x9, x9, 1\n"
              "    blt  x9, x8, loop\n";
    const UnrollResult result = unrollBy2(source);
    ASSERT_EQ(result.loops.size(), 1u);
    ASSERT_TRUE(result.loops[0].unrolled) << result.loops[0].reason;
    bool borrowed = false;
    for (unsigned reg : result.loops[0].renamedTo)
    {
        borrowed = borrowed || isCalleeSaved(reg);
    }
    EXPECT_TRUE(borrowed);
}
