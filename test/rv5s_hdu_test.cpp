// Regression tests for the hazard detection unit.
//
// The unit used to compare the bits at the rs1/rs2 positions of every instruction with the
// destination of the instructions ahead of it. In the I, S, U and J formats those bits are part of
// an immediate, so `lw x5, 8(x10)` right after a write to x8 stalled as if it read x8.

#include <gtest/gtest.h>

#include <string>

#include "optimizer_test_support.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "utils/utils.h"

using namespace Kites;
using namespace optimizer_test;

namespace
{
// `producer` writes x8 and `consumer` follows it directly. The setup is flushed with NOPs so that
// x10 has long settled and the only dependency that can matter is the one on x8.
std::string program(const std::string &producer, const std::string &consumer)
{
    return "li x10, 8192\n addi x0, x0, 0\n addi x0, x0, 0\n addi x0, x0, 0\n addi x0, x0, 0\n" +
           producer + "\n" + consumer + "\n";
}

template <typename Vm> unsigned cyclesOf(const std::string &source)
{
    return runOn<Vm>(assembleSource(source)).cycles;
}
} // namespace

class HazardUnitTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        setupVmStateDirectory();
    }
};

TEST_F(HazardUnitTest, ImmediateBitsAreNotTreatedAsASourceRegisterWithoutForwarding)
{
    const unsigned colliding = cyclesOf<RV5StageProcessorHNF>(program("addi x8, x0, 1", "lw x5, 8(x10)"));
    const unsigned harmless = cyclesOf<RV5StageProcessorHNF>(program("addi x8, x0, 1", "lw x5, 12(x10)"));
    EXPECT_EQ(colliding, harmless);
}

TEST_F(HazardUnitTest, ImmediateBitsAreNotTreatedAsASourceRegisterWithForwarding)
{
    // Only a load can stall with forwarding, so the producer is a load here.
    const unsigned colliding = cyclesOf<RV5StageProcessorHF>(program("lw x8, 0(x10)", "lw x5, 8(x10)"));
    const unsigned harmless = cyclesOf<RV5StageProcessorHF>(program("lw x8, 0(x10)", "lw x5, 12(x10)"));
    EXPECT_EQ(colliding, harmless);
}

TEST_F(HazardUnitTest, AddiImmediateBitsAreNotASourceRegister)
{
    const unsigned colliding = cyclesOf<RV5StageProcessorHNF>(program("addi x8, x0, 1", "addi x5, x10, 8"));
    const unsigned harmless = cyclesOf<RV5StageProcessorHNF>(program("addi x8, x0, 1", "addi x5, x10, 12"));
    EXPECT_EQ(colliding, harmless);
}

TEST_F(HazardUnitTest, RealDependenciesStillStall)
{
    // Guards against over-correcting: a genuine read of the just-written register must cost
    // cycles, two without forwarding and one for a load-use with it.
    const unsigned independentNoForwarding =
        cyclesOf<RV5StageProcessorHNF>(program("addi x8, x0, 1", "add x5, x10, x10"));
    const unsigned dependentNoForwarding =
        cyclesOf<RV5StageProcessorHNF>(program("addi x8, x0, 1", "add x5, x8, x10"));
    EXPECT_EQ(dependentNoForwarding, independentNoForwarding + 2);

    const unsigned independentForwarding =
        cyclesOf<RV5StageProcessorHF>(program("lw x8, 0(x10)", "add x5, x10, x10"));
    const unsigned dependentForwarding =
        cyclesOf<RV5StageProcessorHF>(program("lw x8, 0(x10)", "add x5, x8, x10"));
    EXPECT_EQ(dependentForwarding, independentForwarding + 1);
}

TEST_F(HazardUnitTest, StoreAndBranchOperandsStillStall)
{
    // sw reads x8 as its data (rs2) and beq reads it as rs1: both are real reads.
    const unsigned storeIndependent =
        cyclesOf<RV5StageProcessorHNF>(program("addi x8, x0, 1", "sw x10, 0(x10)"));
    const unsigned storeDependent =
        cyclesOf<RV5StageProcessorHNF>(program("addi x8, x0, 1", "sw x8, 0(x10)"));
    EXPECT_EQ(storeDependent, storeIndependent + 2);

    const unsigned branchIndependent =
        cyclesOf<RV5StageProcessorHNF>(program("addi x8, x0, 1", "beq x10, x0, 4"));
    const unsigned branchDependent =
        cyclesOf<RV5StageProcessorHNF>(program("addi x8, x0, 1", "beq x8, x0, 4"));
    EXPECT_EQ(branchDependent, branchIndependent + 2);
}
