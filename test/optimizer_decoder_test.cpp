#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "common/register_names.h"
#include "optimizer/instr_info.h"

using namespace Kites;
using namespace Kites::optimizer;

namespace
{
// ---------------------------------------------------------------------------
// Instruction encoders. They build words from fields, so the expected encodings in the tests
// do not depend on the assembler.
// ---------------------------------------------------------------------------
uint32_t encR(uint32_t funct7, uint32_t rs2, uint32_t rs1, uint32_t funct3, uint32_t rd,
              uint32_t opcode)
{
    return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode;
}

uint32_t encI(int32_t imm, uint32_t rs1, uint32_t funct3, uint32_t rd, uint32_t opcode)
{
    return ((static_cast<uint32_t>(imm) & 0xFFF) << 20) | (rs1 << 15) | (funct3 << 12) |
           (rd << 7) | opcode;
}

uint32_t encS(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t funct3, uint32_t opcode)
{
    const uint32_t u = static_cast<uint32_t>(imm);
    return (((u >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) |
           ((u & 0x1F) << 7) | opcode;
}

uint32_t encB(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t funct3)
{
    const uint32_t u = static_cast<uint32_t>(imm);
    return (((u >> 12) & 0x1) << 31) | (((u >> 5) & 0x3F) << 25) | (rs2 << 20) | (rs1 << 15) |
           (funct3 << 12) | (((u >> 1) & 0xF) << 8) | (((u >> 11) & 0x1) << 7) | 0b1100011;
}

uint32_t encU(uint32_t imm20, uint32_t rd, uint32_t opcode)
{
    return (imm20 << 12) | (rd << 7) | opcode;
}

uint32_t encJ(int32_t imm, uint32_t rd)
{
    const uint32_t u = static_cast<uint32_t>(imm);
    return (((u >> 20) & 0x1) << 31) | (((u >> 1) & 0x3FF) << 21) | (((u >> 11) & 0x1) << 20) |
           (((u >> 12) & 0xFF) << 12) | (rd << 7) | 0b1101111;
}

uint32_t encR4(uint32_t rs3, uint32_t fmt, uint32_t rs2, uint32_t rs1, uint32_t rd,
               uint32_t opcode)
{
    return (rs3 << 27) | (fmt << 25) | (rs2 << 20) | (rs1 << 15) | (0b111u << 12) | (rd << 7) |
           opcode;
}

constexpr uint32_t kOpReg = 0b0110011;
constexpr uint32_t kOpImm = 0b0010011;
constexpr uint32_t kLoad = 0b0000011;
constexpr uint32_t kStore = 0b0100011;
constexpr uint32_t kFpLoad = 0b0000111;
constexpr uint32_t kFpStore = 0b0100111;
constexpr uint32_t kOpFp = 0b1010011;
constexpr uint32_t kSystem = 0b1110011;

RegRef x(uint8_t index)
{
    return {RegFile::Gpr, index};
}

RegRef f(uint8_t index)
{
    return {RegFile::Fpr, index};
}

std::vector<RegRef> sourcesOf(const InstrInfo &info)
{
    return {info.sources.begin(), info.sources.begin() + info.sourceCount};
}
} // namespace

// ---------------------------------------------------------------------------
// Integer instructions
// ---------------------------------------------------------------------------
TEST(OptimizerDecoder, RTypeReadsTwoSourcesAndWritesDest)
{
    const InstrInfo info = decodeInstruction(encR(0, 2, 1, 0, 3, kOpReg), 0); // add x3, x1, x2
    EXPECT_TRUE(info.isValid);
    EXPECT_EQ(info.klass, InstrClass::Alu);
    EXPECT_EQ(sourcesOf(info), (std::vector<RegRef>{x(1), x(2)}));
    EXPECT_EQ(info.dest, x(3));
    EXPECT_FALSE(info.endsBlock);
    EXPECT_FALSE(info.isBarrier);
}

TEST(OptimizerDecoder, MulAndDivAreMulDivClass)
{
    EXPECT_EQ(decodeInstruction(encR(1, 2, 1, 0, 3, kOpReg), 0).klass, InstrClass::MulDiv); // mul
    EXPECT_EQ(decodeInstruction(encR(1, 2, 1, 4, 3, kOpReg), 0).klass, InstrClass::MulDiv); // div
    EXPECT_EQ(decodeInstruction(encR(1, 2, 1, 0, 3, 0b0111011), 0).klass,
              InstrClass::MulDiv); // mulw
}

TEST(OptimizerDecoder, DuplicateSourceIsListedOnce)
{
    const InstrInfo info = decodeInstruction(encR(0, 2, 2, 0, 1, kOpReg), 0); // add x1, x2, x2
    EXPECT_EQ(sourcesOf(info), (std::vector<RegRef>{x(2)}));
}

TEST(OptimizerDecoder, ZeroRegisterIsNeverASourceOrDest)
{
    const InstrInfo readsZero = decodeInstruction(encR(0, 2, 0, 0, 3, kOpReg), 0); // add x3,x0,x2
    EXPECT_EQ(sourcesOf(readsZero), (std::vector<RegRef>{x(2)}));

    const InstrInfo writesZero = decodeInstruction(encR(0, 2, 1, 0, 0, kOpReg), 0); // add x0,x1,x2
    EXPECT_FALSE(writesZero.dest.isValid());
    EXPECT_EQ(writesZero.sourceCount, 2u);
}

TEST(OptimizerDecoder, NopHasNoDependencies)
{
    const InstrInfo info = decodeInstruction(0x00000013, 0); // addi x0, x0, 0
    EXPECT_TRUE(info.isValid);
    EXPECT_EQ(info.sourceCount, 0u);
    EXPECT_FALSE(info.dest.isValid());

    const InstrInfo zero = decodeInstruction(0x00000000, 0);
    EXPECT_TRUE(zero.isValid);
    EXPECT_EQ(zero.sourceCount, 0u);
}

TEST(OptimizerDecoder, ImmediateInstructionHasOneSourceAndSignedImmediate)
{
    const InstrInfo info = decodeInstruction(encI(-12, 6, 0, 5, kOpImm), 0); // addi x5, x6, -12
    EXPECT_EQ(sourcesOf(info), (std::vector<RegRef>{x(6)}));
    EXPECT_EQ(info.dest, x(5));
    EXPECT_EQ(info.imm, -12);
}

TEST(OptimizerDecoder, LoadDecodesBaseOffsetAndSize)
{
    const InstrInfo lw = decodeInstruction(encI(8, 6, 2, 5, kLoad), 0); // lw x5, 8(x6)
    EXPECT_EQ(lw.klass, InstrClass::Load);
    EXPECT_EQ(lw.memSize, 4);
    EXPECT_EQ(lw.memBase, x(6));
    EXPECT_EQ(lw.imm, 8);
    EXPECT_EQ(sourcesOf(lw), (std::vector<RegRef>{x(6)}));
    EXPECT_EQ(lw.dest, x(5));

    EXPECT_EQ(decodeInstruction(encI(0, 6, 0, 5, kLoad), 0).memSize, 1);  // lb
    EXPECT_EQ(decodeInstruction(encI(0, 6, 1, 5, kLoad), 0).memSize, 2);  // lh
    EXPECT_EQ(decodeInstruction(encI(0, 6, 3, 5, kLoad), 0).memSize, 8);  // ld
    EXPECT_EQ(decodeInstruction(encI(0, 6, 4, 5, kLoad), 0).memSize, 1);  // lbu
    EXPECT_EQ(decodeInstruction(encI(0, 6, 6, 5, kLoad), 0).memSize, 4);  // lwu
}

TEST(OptimizerDecoder, LoadFromAbsoluteAddressKeepsZeroBase)
{
    const InstrInfo info = decodeInstruction(encI(256, 0, 2, 5, kLoad), 0); // lw x5, 256(x0)
    EXPECT_EQ(info.memBase, x(0));
    EXPECT_EQ(info.sourceCount, 0u);
}

TEST(OptimizerDecoder, StoreReadsBaseAndData)
{
    const InstrInfo info = decodeInstruction(encS(-4, 7, 6, 2, kStore), 0); // sw x7, -4(x6)
    EXPECT_EQ(info.klass, InstrClass::Store);
    EXPECT_EQ(info.memSize, 4);
    EXPECT_EQ(info.imm, -4);
    EXPECT_EQ(info.memBase, x(6));
    EXPECT_EQ(sourcesOf(info), (std::vector<RegRef>{x(6), x(7)}));
    EXPECT_FALSE(info.dest.isValid());

    EXPECT_EQ(decodeInstruction(encS(0, 7, 6, 3, kStore), 0).memSize, 8); // sd
}

TEST(OptimizerDecoder, InvalidLoadAndStoreWidthsAreUnknown)
{
    EXPECT_FALSE(decodeInstruction(encI(0, 6, 7, 5, kLoad), 0).isValid);
    EXPECT_FALSE(decodeInstruction(encS(0, 7, 6, 4, kStore), 0).isValid);
}

TEST(OptimizerDecoder, BranchResolvesTargetAndEndsBlock)
{
    const InstrInfo fwd = decodeInstruction(encB(16, 2, 1, 0), 0x100); // beq x1, x2, +16
    EXPECT_EQ(fwd.klass, InstrClass::Branch);
    EXPECT_TRUE(fwd.hasTarget);
    EXPECT_EQ(fwd.targetPc, 0x110u);
    EXPECT_TRUE(fwd.endsBlock);
    EXPECT_EQ(sourcesOf(fwd), (std::vector<RegRef>{x(1), x(2)}));
    EXPECT_FALSE(fwd.dest.isValid());

    const InstrInfo back = decodeInstruction(encB(-8, 2, 1, 1), 0x100); // bne x1, x2, -8
    EXPECT_EQ(back.targetPc, 0xF8u);
    EXPECT_EQ(back.imm, -8);
}

TEST(OptimizerDecoder, BranchWithReservedFunct3IsUnknown)
{
    EXPECT_FALSE(decodeInstruction(encB(8, 2, 1, 2), 0).isValid);
}

TEST(OptimizerDecoder, LuiHasNoSourcesAndFullImmediate)
{
    const InstrInfo info = decodeInstruction(encU(0x12345, 5, 0b0110111), 0); // lui x5, 0x12345
    EXPECT_EQ(info.sourceCount, 0u);
    EXPECT_EQ(info.dest, x(5));
    EXPECT_EQ(info.imm, 0x12345000);
    EXPECT_FALSE(info.readsPc);

    const InstrInfo negative = decodeInstruction(encU(0xFFFFF, 5, 0b0110111), 0);
    EXPECT_EQ(negative.imm, static_cast<int32_t>(0xFFFFF000));
}

TEST(OptimizerDecoder, AuipcReadsPc)
{
    const InstrInfo info = decodeInstruction(encU(1, 5, 0b0010111), 0x40); // auipc x5, 1
    EXPECT_TRUE(info.readsPc);
    EXPECT_EQ(info.sourceCount, 0u);
    EXPECT_EQ(info.dest, x(5));
}

TEST(OptimizerDecoder, JalResolvesTargetAndEndsBlock)
{
    const InstrInfo fwd = decodeInstruction(encJ(2048, 1), 0x10); // jal x1, +2048
    EXPECT_EQ(fwd.klass, InstrClass::Jal);
    EXPECT_EQ(fwd.targetPc, 0x10u + 2048);
    EXPECT_EQ(fwd.sourceCount, 0u);
    EXPECT_EQ(fwd.dest, x(1));
    EXPECT_TRUE(fwd.endsBlock);

    const InstrInfo back = decodeInstruction(encJ(-16, 0), 0x40); // j -16
    EXPECT_EQ(back.targetPc, 0x30u);
    EXPECT_FALSE(back.dest.isValid());
}

TEST(OptimizerDecoder, JalrReadsBaseAndHasNoStaticTarget)
{
    const InstrInfo info = decodeInstruction(encI(4, 5, 0, 1, 0b1100111), 0); // jalr x1, 4(x5)
    EXPECT_EQ(info.klass, InstrClass::Jalr);
    EXPECT_FALSE(info.hasTarget);
    EXPECT_EQ(sourcesOf(info), (std::vector<RegRef>{x(5)}));
    EXPECT_EQ(info.dest, x(1));
    EXPECT_TRUE(info.endsBlock);
}

TEST(OptimizerDecoder, EcallIsABarrierThatReadsSyscallRegisters)
{
    const InstrInfo info = decodeInstruction(0x00000073, 0);
    EXPECT_EQ(info.klass, InstrClass::System);
    EXPECT_TRUE(info.isBarrier);
    EXPECT_TRUE(info.endsBlock);
    EXPECT_EQ(sourcesOf(info), (std::vector<RegRef>{x(17), x(10), x(11), x(12)}));
    EXPECT_EQ(info.dest, x(10));
}

TEST(OptimizerDecoder, EbreakIsABarrierThatEndsBlock)
{
    const InstrInfo info = decodeInstruction(0x00100073, 0);
    EXPECT_EQ(info.klass, InstrClass::System);
    EXPECT_TRUE(info.isBarrier);
    EXPECT_TRUE(info.endsBlock);
}

TEST(OptimizerDecoder, CsrInstructionsAreBarriersButDoNotEndBlock)
{
    const InstrInfo reg = decodeInstruction(encI(0x300, 6, 1, 5, kSystem), 0); // csrrw x5,0x300,x6
    EXPECT_TRUE(reg.isBarrier);
    EXPECT_FALSE(reg.endsBlock);
    EXPECT_EQ(sourcesOf(reg), (std::vector<RegRef>{x(6)}));
    EXPECT_EQ(reg.dest, x(5));

    const InstrInfo imm = decodeInstruction(encI(0x300, 6, 5, 5, kSystem), 0); // csrrwi
    EXPECT_TRUE(imm.isBarrier);
    EXPECT_EQ(imm.sourceCount, 0u);
}

// ---------------------------------------------------------------------------
// Floating point
// ---------------------------------------------------------------------------
TEST(OptimizerDecoder, FpLoadUsesGprBaseAndFprDest)
{
    const InstrInfo info = decodeInstruction(encI(8, 2, 2, 1, kFpLoad), 0); // flw f1, 8(x2)
    EXPECT_EQ(info.klass, InstrClass::Load);
    EXPECT_EQ(info.memSize, 4);
    EXPECT_EQ(info.memBase, x(2));
    EXPECT_EQ(sourcesOf(info), (std::vector<RegRef>{x(2)}));
    EXPECT_EQ(info.dest, f(1));

    EXPECT_EQ(decodeInstruction(encI(0, 2, 3, 1, kFpLoad), 0).memSize, 8); // fld
}

TEST(OptimizerDecoder, FpStoreReadsGprBaseAndFprData)
{
    const InstrInfo info = decodeInstruction(encS(4, 3, 2, 2, kFpStore), 0); // fsw f3, 4(x2)
    EXPECT_EQ(info.klass, InstrClass::Store);
    EXPECT_EQ(sourcesOf(info), (std::vector<RegRef>{x(2), f(3)}));
    EXPECT_FALSE(info.dest.isValid());

    EXPECT_EQ(decodeInstruction(encS(0, 3, 2, 3, kFpStore), 0).memSize, 8); // fsd
}

TEST(OptimizerDecoder, FpArithmeticUsesFprOnly)
{
    const InstrInfo single = decodeInstruction(encR(0b0000000, 3, 2, 7, 1, kOpFp), 0); // fadd.s
    EXPECT_EQ(single.klass, InstrClass::FloatingPoint);
    EXPECT_EQ(sourcesOf(single), (std::vector<RegRef>{f(2), f(3)}));
    EXPECT_EQ(single.dest, f(1));

    const InstrInfo dbl = decodeInstruction(encR(0b0000001, 3, 2, 7, 1, kOpFp), 0); // fadd.d
    EXPECT_EQ(sourcesOf(dbl), (std::vector<RegRef>{f(2), f(3)}));
    EXPECT_EQ(dbl.dest, f(1));
}

TEST(OptimizerDecoder, FsqrtHasOneSource)
{
    const InstrInfo info = decodeInstruction(encR(0b0101100, 0, 2, 7, 1, kOpFp), 0); // fsqrt.s
    EXPECT_EQ(sourcesOf(info), (std::vector<RegRef>{f(2)}));
    EXPECT_EQ(info.dest, f(1));
}

TEST(OptimizerDecoder, FpCompareWritesAGpr)
{
    const InstrInfo info = decodeInstruction(encR(0b1010000, 2, 1, 2, 5, kOpFp), 0); // feq.s x5
    EXPECT_EQ(sourcesOf(info), (std::vector<RegRef>{f(1), f(2)}));
    EXPECT_EQ(info.dest, x(5));
}

TEST(OptimizerDecoder, FpToIntConversionCrossesRegisterFiles)
{
    const InstrInfo toInt = decodeInstruction(encR(0b1100000, 0, 1, 7, 5, kOpFp), 0); // fcvt.w.s
    EXPECT_EQ(sourcesOf(toInt), (std::vector<RegRef>{f(1)}));
    EXPECT_EQ(toInt.dest, x(5));

    const InstrInfo fromInt = decodeInstruction(encR(0b1101000, 0, 5, 7, 1, kOpFp), 0); // fcvt.s.w
    EXPECT_EQ(sourcesOf(fromInt), (std::vector<RegRef>{x(5)}));
    EXPECT_EQ(fromInt.dest, f(1));
}

TEST(OptimizerDecoder, FpMoveCrossesRegisterFiles)
{
    const InstrInfo toGpr = decodeInstruction(encR(0b1110000, 0, 1, 0, 5, kOpFp), 0); // fmv.x.w
    EXPECT_EQ(sourcesOf(toGpr), (std::vector<RegRef>{f(1)}));
    EXPECT_EQ(toGpr.dest, x(5));

    const InstrInfo toFpr = decodeInstruction(encR(0b1111000, 0, 5, 0, 1, kOpFp), 0); // fmv.w.x
    EXPECT_EQ(sourcesOf(toFpr), (std::vector<RegRef>{x(5)}));
    EXPECT_EQ(toFpr.dest, f(1));
}

TEST(OptimizerDecoder, FmaReadsThreeFprs)
{
    const InstrInfo info = decodeInstruction(encR4(4, 0, 3, 2, 1, 0b1000011), 0); // fmadd.s
    EXPECT_EQ(sourcesOf(info), (std::vector<RegRef>{f(2), f(3), f(4)}));
    EXPECT_EQ(info.dest, f(1));
}

TEST(OptimizerDecoder, FprZeroIsARealRegister)
{
    const InstrInfo info = decodeInstruction(encR(0b0000000, 0, 0, 7, 0, kOpFp), 0); // fadd.s f0,f0,f0
    EXPECT_EQ(sourcesOf(info), (std::vector<RegRef>{f(0)}));
    EXPECT_EQ(info.dest, f(0));
}

TEST(OptimizerDecoder, SameNumberInDifferentFilesIsDifferentRegister)
{
    EXPECT_NE(x(5), f(5));
}

// ---------------------------------------------------------------------------
// Unrecognised words
// ---------------------------------------------------------------------------
TEST(OptimizerDecoder, UnknownWordIsAConservativeBarrier)
{
    const InstrInfo info = decodeInstruction(0xFFFFFFFF, 0);
    EXPECT_FALSE(info.isValid);
    EXPECT_EQ(info.klass, InstrClass::Unknown);
    EXPECT_TRUE(info.isBarrier);
    EXPECT_TRUE(info.endsBlock);
    EXPECT_EQ(info.sourceCount, 0u);
    EXPECT_FALSE(info.dest.isValid());
}

// ---------------------------------------------------------------------------
// Oracle: the decoder must agree with the assembler on which registers each instruction uses.
// ---------------------------------------------------------------------------
namespace
{
std::optional<RegRef> parseRegisterName(const std::string &name)
{
    for (size_t i = 0; i < 32; ++i)
    {
        if (name == register_names::kGprNames[i] || name == register_names::kGprAliases[i])
        {
            return RegRef{RegFile::Gpr, static_cast<uint8_t>(i)};
        }
        if (name == register_names::kFprNames[i] || name == register_names::kFprAliases[i])
        {
            return RegRef{RegFile::Fpr, static_cast<uint8_t>(i)};
        }
    }
    return std::nullopt;
}

using RegKey = std::pair<int, int>;

RegKey keyOf(RegRef reg)
{
    return {static_cast<int>(reg.file), reg.index};
}

// Registers the assembler recorded for one instruction, excluding x0.
std::set<RegKey> assemblerRegisters(const ICUnit &unit)
{
    std::set<RegKey> result;
    for (const auto *field : {&unit.rd, &unit.rs1, &unit.rs2, &unit.rs3})
    {
        if ((*field)[0] == '\0')
        {
            continue;
        }
        const auto reg = parseRegisterName(field->data());
        if (reg && !(reg->file == RegFile::Gpr && reg->index == 0))
        {
            result.insert(keyOf(*reg));
        }
    }
    return result;
}

std::set<RegKey> decodedRegisters(const InstrInfo &info)
{
    std::set<RegKey> result;
    for (size_t i = 0; i < info.sourceCount; ++i)
    {
        result.insert(keyOf(info.sources[i]));
    }
    if (info.dest.isValid())
    {
        result.insert(keyOf(info.dest));
    }
    return result;
}

// Returns the number of instructions compared.
size_t compareWithAssembler(AssembledProgram &program, const std::string &label)
{
    EXPECT_EQ(program.intermediate_code.size(), program.text_buffer.size()) << label;
    if (program.intermediate_code.size() != program.text_buffer.size())
    {
        return 0;
    }

    size_t compared = 0;
    for (size_t i = 0; i < program.text_buffer.size(); ++i)
    {
        const ICUnit &unit = program.intermediate_code[i].first;
        const InstrInfo info = decodeInstruction(program.text_buffer[i], i * 4);
        EXPECT_TRUE(info.isValid) << label << ": instruction " << i << " (" << unit.opcode.data()
                                  << ") did not decode";
        if (!info.isValid || info.klass == InstrClass::System)
        {
            continue; // ecall/ebreak/CSR operands are implicit or handled separately
        }
        EXPECT_EQ(decodedRegisters(info), assemblerRegisters(unit))
            << label << ": instruction " << i << " (" << unit.opcode.data() << ")";
        ++compared;
    }
    return compared;
}
} // namespace

TEST(OptimizerDecoderOracle, AgreesWithAssemblerOnMixedProgram)
{
    std::istringstream source(R"(
        add   x3, x1, x2
        sub   a0, a1, a2
        mul   t0, t1, t2
        addi  s0, s1, -5
        slli  x5, x6, 3
        lui   x7, 0x12345
        auipc x8, 1
        lw    x9, 8(x10)
        lbu   x11, 0(x12)
        sw    x13, -4(x14)
        sd    x15, 16(x2)
        beq   x1, x2, done
        bne   x3, x0, done
        blt   x4, x5, done
        jal   x1, done
        jalr  x1, 0(x5)
        flw   f1, 8(x2)
        fsw   f3, 4(x2)
        fld   f4, 0(x2)
        fsd   f5, 8(x2)
        fadd.s f1, f2, f3
        fmul.d f4, f5, f6
        fsqrt.s f1, f2
        fmin.s f1, f2, f3
        fsgnj.s f1, f2, f3
        feq.s x5, f1, f2
        flt.d x6, f1, f2
        fcvt.w.s x5, f1
        fcvt.s.w f1, x5
        fmv.x.w x5, f1
        fmv.w.x f1, x5
        fmadd.s f1, f2, f3, f4
        fmsub.d f1, f2, f3, f4
    done:
        addi x0, x0, 0
    )");
    AssembledProgram program = assemble(source);
    ASSERT_FALSE(program.text_buffer.empty());
    EXPECT_GT(compareWithAssembler(program, "mixed program"), 20u);
}

TEST(OptimizerDecoderOracle, AgreesWithAssemblerOnAllExamplePrograms)
{
    size_t programCount = 0;
    size_t instructionCount = 0;

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
            AssembledProgram program = assemble(file);
            if (program.text_buffer.empty())
            {
                continue; // did not assemble; not this test's concern
            }
            instructionCount += compareWithAssembler(program, entry.path().filename().string());
            ++programCount;
        }
    }

    EXPECT_GT(programCount, 10u);
    EXPECT_GT(instructionCount, 100u);
}
