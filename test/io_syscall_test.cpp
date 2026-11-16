// I/O through ecall, on every processor model.
//
// The pipelines without hazard detection (NH_*) leave dependent instructions to the programmer, so
// every program here is padded with nops after each instruction by `pad`. That also checks that
// an ecall needs no padding of its own beyond what any other instruction needs.

#include <gtest/gtest.h>

#include <memory>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rv5s/rv5s_processor_nh_f.h"
#include "processor/rv5s/rv5s_processor_nh_nf.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

using namespace Kites;

namespace
{
// Puts three nops after every instruction of a straight-line program (labels and directives are
// left alone).
std::string pad(const std::string &source)
{
    std::istringstream in(source);
    std::string line;
    std::string out;
    while (std::getline(in, line))
    {
        out += line + "\n";
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#' || line[first] == '.' ||
            line.find(':') != std::string::npos)
        {
            continue;
        }
        out += "nop\nnop\nnop\n";
    }
    return out;
}

template <typename VmType> std::unique_ptr<VmType> load(const std::string &source)
{
    setupVmStateDirectory();
    auto vm = std::make_unique<VmType>();
    std::istringstream stream(pad(source));
    vm->LoadProgram(assemble(stream));
    return vm;
}

template <typename VmType>
std::unique_ptr<VmType> run(const std::string &source, const std::string &input = "")
{
    auto vm = load<VmType>(source);
    vm->Io().pushRaw(input);
    vm->DebugRun();
    return vm;
}

template <typename VmType> class IoTest : public ::testing::Test
{
};

using Processors = ::testing::Types<RVSSProcessor, RV5StageProcessorNHNF, RV5StageProcessorHNF,
                                    RV5StageProcessorNHF, RV5StageProcessorHF>;
TYPED_TEST_SUITE(IoTest, Processors);

// Data at 0x10000000, which `lui reg, 0x10000` reaches in one instruction.
constexpr const char *kText = ".data\nmsg: .string \"hi there\\n\"\n.text\n";
} // namespace

TYPED_TEST(IoTest, PrintsIntegerCharAndString)
{
    auto vm = run<TypeParam>(std::string(kText) + R"(
        li   a0, -42
        li   a7, 1
        ecall
        li   a0, 32
        li   a7, 11
        ecall
        lui  a0, 0x10000
        li   a7, 4
        ecall
    )");
    EXPECT_EQ(vm->Io().output(), "-42 hi there\n");
}

TYPED_TEST(IoTest, WriteSyscallPrintsABuffer)
{
    auto vm = run<TypeParam>(std::string(kText) + R"(
        li   a0, 1
        lui  a1, 0x10000
        li   a2, 2
        li   a7, 64
        ecall
        mv   s0, a0
    )");
    EXPECT_EQ(vm->Io().output(), "hi");
    EXPECT_EQ(vm->registers_.ReadGpr(8), 2u); // write returns the count
}

TYPED_TEST(IoTest, ReadsIntegersSeparatedByAnyWhiteSpace)
{
    auto vm = run<TypeParam>(R"(
        li   a7, 5
        ecall
        mv   s0, a0
        li   a7, 5
        ecall
        mv   s1, a0
    )",
                             "  17 \n -3\n");
    EXPECT_EQ(vm->registers_.ReadGpr(8), 17u);
    EXPECT_EQ(static_cast<int64_t>(vm->registers_.ReadGpr(9)), -3);
}

// The instruction right behind a read used to be able to see the old a0.
TYPED_TEST(IoTest, InstructionAfterAReadSeesTheResult)
{
    auto vm = run<TypeParam>(R"(
        li   a0, 7
        li   a7, 5
        ecall
        addi s0, a0, 1
    )",
                             "41\n");
    EXPECT_EQ(vm->registers_.ReadGpr(8), 42u);
}

TYPED_TEST(IoTest, ReadsAndPrintsFloats)
{
    auto vm = run<TypeParam>(R"(
        li   a7, 6
        ecall
        fadd.s ft0, fa0, fa0
        fsgnj.s fa0, ft0, ft0
        li   a7, 2
        ecall
    )",
                             "1.25\n");
    EXPECT_EQ(vm->Io().output(), "2.5");
}

TYPED_TEST(IoTest, ReadsACharacterAndAString)
{
    auto vm = run<TypeParam>(R"(
        li   a7, 12
        ecall
        mv   s0, a0
        lui  a0, 0x11000
        li   a1, 16
        li   a7, 8
        ecall
        lui  a0, 0x11000
        li   a7, 4
        ecall
    )",
                             "xhello\n");
    EXPECT_EQ(vm->registers_.ReadGpr(8), static_cast<uint64_t>('x'));
    EXPECT_EQ(vm->Io().output(), "hello\n");
}

TYPED_TEST(IoTest, ReadSyscallFillsTheBuffer)
{
    auto vm = run<TypeParam>(R"(
        li   a0, 0
        lui  a1, 0x11000
        li   a2, 64
        li   a7, 63
        ecall
        mv   s0, a0
    )",
                             "abc\n");
    EXPECT_EQ(vm->registers_.ReadGpr(8), 4u); // "abc\n"
    EXPECT_EQ(vm->memory_controller_.readByte(0x11000000), 'a');
    EXPECT_EQ(vm->memory_controller_.readByte(0x11000003), '\n');
}

TYPED_TEST(IoTest, ExitEndsTheProgram)
{
    auto vm = run<TypeParam>(R"(
        li   s0, 1
        li   a7, 10
        ecall
        li   s0, 2
        li   s1, 3
    )");
    EXPECT_EQ(vm->registers_.ReadGpr(8), 1u); // what is after the exit never ran
    EXPECT_EQ(vm->registers_.ReadGpr(9), 0u);
    EXPECT_TRUE(vm->exited_);
    EXPECT_EQ(vm->exit_code_, 0);
}

TYPED_TEST(IoTest, Exit93TakesTheCodeFromA0)
{
    auto vm = run<TypeParam>(R"(
        li   a0, 3
        li   a7, 93
        ecall
    )");
    EXPECT_TRUE(vm->exited_);
    EXPECT_EQ(vm->exit_code_, 3);
}

TYPED_TEST(IoTest, UnknownSyscallIsIgnored)
{
    auto vm = run<TypeParam>(R"(
        li   a7, 999
        ecall
        li   s0, 5
    )");
    EXPECT_EQ(vm->registers_.ReadGpr(8), 5u);
    EXPECT_EQ(vm->Io().output(), "");
}

// ---- Memory-mapped console ------------------------------------------------------------------

TYPED_TEST(IoTest, KeyboardAndDisplayRegisters)
{
    // Waits for a key, echoes it, until 'q'.
    auto vm = run<TypeParam>(R"(
        lui  t0, 0x20000
        li   t3, 113
    wait:
        lw   t1, 0(t0)
        andi t1, t1, 1
        beq  t1, x0, wait
        lw   t2, 4(t0)
        sw   t2, 12(t0)
        bne  t2, t3, wait
    )",
                             "ab q");
    EXPECT_EQ(vm->Io().output(), "ab q");
}

TYPED_TEST(IoTest, DeviceAccessesNeverTouchTheCaches)
{
    auto vm = run<TypeParam>(R"(
        lui  t0, 0x20000
        li   t1, 65
        sw   t1, 12(t0)
        lw   t2, 8(t0)
    )");
    EXPECT_EQ(vm->Io().output(), "A");
    EXPECT_EQ(vm->registers_.ReadGpr(7), 1u); // the display is always ready
    EXPECT_EQ(vm->memory_controller_.getL1Cache()->getHitCount() +
                  vm->memory_controller_.getL1Cache()->getMissCount(),
              0u);
}

// ---- Undo and redo --------------------------------------------------------------------------

TYPED_TEST(IoTest, UndoTakesBackOutputAndRedoPrintsItAgain)
{
    auto vm = run<TypeParam>(R"(
        li   a0, 5
        li   a7, 1
        ecall
        li   s0, 9
    )");
    ASSERT_EQ(vm->Io().output(), "5");

    // Undo until the print is gone, but no further than needed.
    int undone = 0;
    while (vm->Io().output() == "5" && undone < 100)
    {
        vm->Undo();
        ++undone;
    }
    EXPECT_EQ(vm->Io().output(), "");
    EXPECT_LT(undone, 100);

    for (int i = 0; i < undone; ++i)
    {
        vm->Redo();
    }
    EXPECT_EQ(vm->Io().output(), "5"); // once, not twice: redo does not run the program again
}

TYPED_TEST(IoTest, UndoGivesBackInputAndRedoTakesItAgain)
{
    auto vm = run<TypeParam>(R"(
        li   a7, 5
        ecall
        mv   s0, a0
    )",
                             "123 456\n");
    ASSERT_EQ(vm->registers_.ReadGpr(8), 123u);
    EXPECT_EQ(vm->Io().pendingInput(), 5u); // " 456\n"

    int undone = 0;
    while (vm->Io().pendingInput() == 5u && undone < 100)
    {
        vm->Undo();
        ++undone;
    }
    EXPECT_EQ(vm->Io().pendingInput(), 8u); // "123 456\n" again
    EXPECT_EQ(vm->registers_.ReadGpr(10), 0u);

    for (int i = 0; i < undone; ++i)
    {
        vm->Redo();
    }
    EXPECT_EQ(vm->Io().pendingInput(), 5u);
    EXPECT_EQ(vm->registers_.ReadGpr(8), 123u);
}

TYPED_TEST(IoTest, UndoOfTheExitLetsTheProgramContinue)
{
    auto vm = run<TypeParam>(R"(
        li   a7, 10
        ecall
    )");
    ASSERT_TRUE(vm->exited_);
    int undone = 0;
    while (vm->exited_ && undone < 100)
    {
        vm->Undo();
        ++undone;
    }
    EXPECT_FALSE(vm->exited_);
}

// ---- Stepping -------------------------------------------------------------------------------

// The GUI's Step runs on the window's thread and must not wait for input.
TYPED_TEST(IoTest, StepWithoutInputDoesNothingUntilInputArrives)
{
    auto vm = load<TypeParam>(R"(
        li   a7, 5
        ecall
        mv   s0, a0
    )");
    vm->Io().setBlocking(false);

    for (int i = 0; i < 40; ++i)
    {
        vm->Step();
    }
    // It reached the ecall and stopped there.
    const auto cyclesWaiting = vm->cycle_s_;
    const auto pcWaiting = vm->program_counter_;
    for (int i = 0; i < 10; ++i)
    {
        vm->Step();
    }
    EXPECT_EQ(vm->cycle_s_, cyclesWaiting);
    EXPECT_EQ(vm->program_counter_, pcWaiting);
    EXPECT_EQ(vm->registers_.ReadGpr(8), 0u);

    vm->Io().pushLine("77");
    for (int i = 0; i < 40; ++i)
    {
        vm->Step();
    }
    EXPECT_EQ(vm->registers_.ReadGpr(8), 77u);
}

// A run that is stopped while it waits must not hang.
TYPED_TEST(IoTest, StoppingWakesAReadThatIsWaiting)
{
    auto vm = load<TypeParam>(R"(
        li   a7, 5
        ecall
    )");
    std::thread stopper(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            vm->stop_requested_ = true;
            vm->Io().wakeReaders();
        });
    vm->DebugRun();
    stopper.join();
    SUCCEED();
}

// A loop that reads on every pass: each ecall flushes what was fetched behind it, which must not
// confuse a branch that is still in flight.
TYPED_TEST(IoTest, ReadsInALoop)
{
    auto vm = run<TypeParam>(R"(
        li   a7, 5
        ecall
        mv   t0, a0
        li   s0, 0
    loop:
        beq  t0, x0, done
        li   a7, 5
        ecall
        add  s0, s0, a0
        addi t0, t0, -1
        j    loop
    done:
        mv   a0, s0
        li   a7, 1
        ecall
    )",
                             "3\n10 20 30\n");
    EXPECT_EQ(vm->Io().output(), "60");
}

TYPED_TEST(IoTest, SumsFloatsInALoop)
{
    auto vm = run<TypeParam>(R"(
        li   a7, 5
        ecall
        mv   t0, a0
        fmv.w.x ft0, x0
    loop:
        beq  t0, x0, done
        li   a7, 6
        ecall
        fadd.s ft0, ft0, fa0
        addi t0, t0, -1
        j    loop
    done:
        fsgnj.s fa0, ft0, ft0
        li   a7, 2
        ecall
    )",
                             "2\n1.5 2.25\n");
    EXPECT_EQ(vm->Io().output(), "3.75");
}

// `la` is auipc + addi. The pipelines used to compute auipc as the immediate alone.
TYPED_TEST(IoTest, AuipcAddsThePc)
{
    auto vm = run<TypeParam>(R"(
        nop
        auipc s0, 1
    )");
    // 3 nops pad the first nop, so the auipc is the fifth instruction: pc = 16.
    EXPECT_EQ(vm->registers_.ReadGpr(8), 0x1000u + 16u);
}
