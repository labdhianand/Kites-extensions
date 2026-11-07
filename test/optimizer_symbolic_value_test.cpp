#include <gtest/gtest.h>

#include <string>

#include "optimizer/program_model.h"
#include "optimizer/symbolic_value.h"
#include "optimizer_test_support.h"

using namespace Kites;
using namespace Kites::optimizer;
using namespace optimizer_test;

namespace
{
// Runs every instruction of `source` through a fresh SymbolicRegisters.
SymbolicRegisters after(const std::string &source)
{
    const ProgramModel model = buildProgramModel(assembleSource(source));
    SymbolicRegisters registers;
    for (size_t k = 0; k < model.instructions.size(); ++k)
    {
        registers.execute(model.instructions[k], 1000 + static_cast<uint32_t>(k));
    }
    return registers;
}

SymbolicValue entry(uint32_t gpr)
{
    return SymbolicValue::ofSymbol(gpr);
}
} // namespace

TEST(OptimizerSymbolicValue, ArithmeticCombinesTermsAndWrapsLikeRegisters)
{
    const SymbolicValue a = entry(10).plus(entry(12).times(4)).plusConstant(8);
    EXPECT_EQ(a.terms.at(10), 1u);
    EXPECT_EQ(a.terms.at(12), 4u);
    EXPECT_EQ(a.constant, 8u);

    // Subtracting a value leaves just the difference, and cancelled terms disappear.
    const SymbolicValue difference = a.minus(entry(10));
    EXPECT_EQ(difference.terms.count(10), 0u);
    EXPECT_EQ(difference.terms.at(12), 4u);

    // Wraparound behaves like a 64-bit register.
    const SymbolicValue wrapped = SymbolicValue::ofConstant(~uint64_t{0}).plusConstant(1);
    EXPECT_EQ(wrapped.constant, 0u);
}

TEST(OptimizerSymbolicValue, ZeroRegisterIsTheConstantZero)
{
    SymbolicRegisters registers;
    EXPECT_TRUE(registers.value(0).terms.empty());
    EXPECT_EQ(registers.value(0).constant, 0u);
    EXPECT_EQ(registers.value(5), entry(5));
}

TEST(OptimizerSymbolicValue, FollowsAddiAddSubSlliAndLui)
{
    const SymbolicRegisters r = after(R"(
        slli x14, x12, 2
        add  x15, x10, x14
        addi x16, x15, 4
        sub  x17, x16, x10
        lui  x18, 2
    )");
    EXPECT_EQ(r.value(14), entry(12).times(4));
    EXPECT_EQ(r.value(15), entry(10).plus(entry(12).times(4)));
    EXPECT_EQ(r.value(16), entry(10).plus(entry(12).times(4)).plusConstant(4));
    EXPECT_EQ(r.value(17), entry(12).times(4).plusConstant(4));
    EXPECT_EQ(r.value(18), SymbolicValue::ofConstant(8192));
}

TEST(OptimizerSymbolicValue, NegativeImmediatesAndLuiAreSignExtended)
{
    const SymbolicRegisters r = after("addi x5, x6, -4\n lui x7, 0xFFFFF\n");
    EXPECT_EQ(r.value(5), entry(6).plusConstant(static_cast<uint64_t>(-4)));
    EXPECT_EQ(r.value(7).constant, static_cast<uint64_t>(int64_t{-4096}));
}

TEST(OptimizerSymbolicValue, AnythingElseGetsItsOwnOpaqueSymbol)
{
    const SymbolicRegisters r = after(R"(
        lw   x5, 0(x10)
        mul  x6, x7, x8
        srli x9, x10, 1
        addi x11, x5, 8
    )");
    EXPECT_EQ(r.value(5), SymbolicValue::ofSymbol(1000));
    EXPECT_EQ(r.value(6), SymbolicValue::ofSymbol(1001));
    EXPECT_EQ(r.value(9), SymbolicValue::ofSymbol(1002));
    // A value derived from an opaque one keeps the same symbol, so the two are comparable.
    EXPECT_EQ(r.value(11), SymbolicValue::ofSymbol(1000).plusConstant(8));
}

TEST(OptimizerSymbolicValue, ThirtyTwoBitOperationsAreOpaque)
{
    // addiw x9, x10, 4 sign-extends a 32-bit sum, which is not linear in 64 bits. Kites' assembler
    // does not accept the `w` instructions, so the word is encoded by hand.
    const uint32_t addiw = (4u << 20) | (10u << 15) | (0u << 12) | (9u << 7) | 0b0011011u;
    SymbolicRegisters registers;
    registers.execute(decodeInstruction(addiw, 0), 77);
    EXPECT_EQ(registers.value(9), SymbolicValue::ofSymbol(77));
}

TEST(OptimizerSymbolicValue, WritingTheSameRegisterReadsTheOldValueFirst)
{
    const SymbolicRegisters r = after("addi x12, x12, 1\n addi x12, x12, 1\n");
    EXPECT_EQ(r.value(12), entry(12).plusConstant(2));
}
