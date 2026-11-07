/**
 * @file loop_unroller.cpp
 * @brief Implementation of the exact loop unroller.
 */
#include "optimizer/loop_unroller.h"

#include <array>
#include <bitset>
#include <map>
#include <sstream>
#include <stdexcept>

#include "assembler/assembler.h"
#include "optimizer/constant_propagation.h"
#include "optimizer/liveness.h"
#include "optimizer/program_model.h"
#include "optimizer/source_text.h"

namespace Kites
{
namespace optimizer
{
namespace
{
constexpr long kMaxSimulatedIterations = 100000;

// Registers x0-x4 (zero, ra, sp, gp, tp) have special roles and are never handed out.
constexpr unsigned kFirstAllocatableRegister = 5;

bool usesFloatingPoint(const InstrInfo &info)
{
    if (info.klass == InstrClass::FloatingPoint || info.dest.file == RegFile::Fpr)
    {
        return true;
    }
    for (size_t s = 0; s < info.sourceCount; ++s)
    {
        if (info.sources[s].file == RegFile::Fpr)
        {
            return true;
        }
    }
    return false;
}

bool branchTaken(uint8_t funct3, uint64_t a, uint64_t b)
{
    switch (funct3)
    {
    case 0b000:
        return a == b;
    case 0b001:
        return a != b;
    case 0b100:
        return static_cast<int64_t>(a) < static_cast<int64_t>(b);
    case 0b101:
        return static_cast<int64_t>(a) >= static_cast<int64_t>(b);
    case 0b110:
        return a < b;
    default:
        return a >= b;
    }
}

// The loop counter: written by exactly one `addi iv, iv, step` and compared by the branch.
struct InductionVariable
{
    unsigned reg{0};
    int64_t step{0};
    unsigned bound{0}; ///< the other branch operand, never written in the loop
};

bool findInductionVariable(const ProgramModel &model, const BasicBlock &block,
                           InductionVariable &result)
{
    const InstrInfo &branch =
        model.instructions[block.firstInstruction + block.instructionCount - 1];
    const unsigned rs1 = (branch.word >> 15) & 0x1F;
    const unsigned rs2 = (branch.word >> 20) & 0x1F;

    std::array<int, 32> writeCount{};
    std::array<const InstrInfo *, 32> writer{};
    for (size_t k = block.firstInstruction; k < block.firstInstruction + block.instructionCount;
         ++k)
    {
        const InstrInfo &info = model.instructions[k];
        if (info.dest.isValid() && info.dest.file == RegFile::Gpr)
        {
            ++writeCount[info.dest.index];
            writer[info.dest.index] = &info;
        }
    }

    auto isStepOf = [&](unsigned reg)
    {
        if (reg == 0 || writeCount[reg] != 1)
        {
            return false;
        }
        const uint32_t word = writer[reg]->word;
        const bool isAddi = (word & 0x7F) == 0b0010011 && ((word >> 12) & 0x7) == 0;
        return isAddi && ((word >> 15) & 0x1F) == reg && writer[reg]->imm != 0;
    };

    for (const auto &[candidate, other] : {std::pair{rs1, rs2}, std::pair{rs2, rs1}})
    {
        if (isStepOf(candidate) && writeCount[other] == 0)
        {
            result = {candidate, writer[candidate]->imm, other};
            return true;
        }
    }
    return false;
}

// Iterations of a do-while loop: the body runs, the counter steps, the branch decides.
long tripCount(uint8_t funct3, bool counterIsFirstOperand, uint64_t start, int64_t step,
               uint64_t bound)
{
    uint64_t counter = start;
    for (long iterations = 1; iterations <= kMaxSimulatedIterations; ++iterations)
    {
        counter += static_cast<uint64_t>(step);
        const bool again = counterIsFirstOperand ? branchTaken(funct3, counter, bound)
                                                 : branchTaken(funct3, bound, counter);
        if (!again)
        {
            return iterations;
        }
    }
    return -1;
}

// Registers an iteration writes before reading them. They carry nothing from one iteration to the
// next, so each copy may use its own.
std::bitset<32> iterationLocalRegisters(const ProgramModel &model, const BasicBlock &block)
{
    std::bitset<32> read;
    std::bitset<32> written;
    std::bitset<32> local;
    for (size_t k = block.firstInstruction; k < block.firstInstruction + block.instructionCount;
         ++k)
    {
        const InstrInfo &info = model.instructions[k];
        for (size_t s = 0; s < info.sourceCount; ++s)
        {
            if (info.sources[s].file == RegFile::Gpr && !written[info.sources[s].index])
            {
                read.set(info.sources[s].index);
            }
        }
        if (info.dest.isValid() && info.dest.file == RegFile::Gpr)
        {
            written.set(info.dest.index);
        }
    }
    for (unsigned r = 1; r < 32; ++r)
    {
        if (written[r] && !read[r])
        {
            local.set(r);
        }
    }
    return local;
}

std::bitset<32> registersUsedAnywhere(const ProgramModel &model)
{
    std::bitset<32> used;
    used.set(0);
    for (const InstrInfo &info : model.instructions)
    {
        for (size_t s = 0; s < info.sourceCount; ++s)
        {
            if (info.sources[s].file == RegFile::Gpr)
            {
                used.set(info.sources[s].index);
            }
        }
        if (info.dest.isValid() && info.dest.file == RegFile::Gpr)
        {
            used.set(info.dest.index);
        }
        if (info.isMemory())
        {
            used.set(info.memBase.index);
        }
    }
    return used;
}

void checkAssembles(const std::string &source, const char *what)
{
    std::istringstream stream(source);
    try
    {
        (void)assemble(stream);
    }
    catch (const std::exception &error)
    {
        throw std::runtime_error(std::string(what) + ": " + error.what());
    }
}
} // namespace

UnrollResult unrollLoops(const std::string &source, const UnrollOptions &options)
{
    std::istringstream stream(source);
    AssembledProgram program;
    try
    {
        program = assemble(stream);
    }
    catch (const std::exception &error)
    {
        throw std::runtime_error(std::string("the program does not assemble: ") + error.what());
    }

    const ProgramModel model = buildProgramModel(program);
    const ControlFlow flow = computeControlFlow(model);
    const std::vector<BlockConstants> constants = propagateConstants(model, flow);
    const std::bitset<32> usedRegisters = registersUsedAnywhere(model);

    std::vector<unsigned> freeRegisters;
    for (unsigned r = 31; r >= kFirstAllocatableRegister; --r)
    {
        if (!usedRegisters[r])
        {
            freeRegisters.push_back(r);
        }
    }

    const SourceText text = splitSource(source);
    std::map<unsigned, std::vector<std::string>> replacements;
    UnrollResult result;

    for (size_t b = 0; b < model.blocks.size(); ++b)
    {
        const BasicBlock &block = model.blocks[b];
        const InstrInfo &last = model.instructions[block.firstInstruction + block.instructionCount - 1];
        const bool loopsToItself = last.klass == InstrClass::Branch && last.hasTarget &&
                                   last.targetPc == model.instructions[block.firstInstruction].pc;
        if (!loopsToItself)
        {
            continue;
        }

        LoopReport report;
        report.firstLine = model.units[block.firstUnit].sourceLine;
        report.lastLine = model.units[block.firstUnit + block.unitCount - 1].sourceLine;
        auto skip = [&](std::string reason)
        {
            report.reason = std::move(reason);
            result.loops.push_back(report);
        };

        if (!block.isReorderable)
        {
            skip("the loop can be entered in the middle of a pseudo-instruction");
            continue;
        }
        bool floatingPoint = false;
        for (size_t k = block.firstInstruction; k < block.firstInstruction + block.instructionCount;
             ++k)
        {
            floatingPoint = floatingPoint || usesFloatingPoint(model.instructions[k]);
        }
        if (floatingPoint)
        {
            skip("floating-point instructions are not supported on the pipelined processor models");
            continue;
        }
        if (flow.hasUnknownPredecessors[b])
        {
            skip("the loop may be entered from an unknown place, so its start values are unknown");
            continue;
        }

        InductionVariable counter;
        if (!findInductionVariable(model, block, counter))
        {
            skip("the branch does not compare a counter stepped once per iteration with a fixed "
                 "bound");
            continue;
        }

        // The registers on entry from outside the loop (the back edge is not an entry).
        std::optional<ConstantRegisters> entry;
        for (size_t predecessor : flow.predecessors[b])
        {
            if (predecessor == b || !constants[predecessor].reachable)
            {
                continue;
            }
            ConstantRegisters exit = constants[predecessor].atExit;
            if (entry)
            {
                for (size_t r = 0; r < 32; ++r)
                {
                    if (!((*entry)[r] && exit[r] && *(*entry)[r] == *exit[r]))
                    {
                        (*entry)[r].reset();
                    }
                }
            }
            else
            {
                entry = exit;
            }
        }
        if (!entry || !(*entry)[counter.reg] || !(*entry)[counter.bound])
        {
            skip("the counter's start value or the bound is not a known constant on entry");
            continue;
        }

        const uint8_t funct3 = (last.word >> 12) & 0x7;
        const bool counterIsFirstOperand = ((last.word >> 15) & 0x1F) == counter.reg;
        report.tripCount = tripCount(funct3, counterIsFirstOperand, *(*entry)[counter.reg],
                                     counter.step, *(*entry)[counter.bound]);
        if (report.tripCount < 0)
        {
            skip("the loop runs too many times to count");
            continue;
        }

        int factor = 1;
        for (int f = options.maxFactor; f >= 2; --f)
        {
            if (report.tripCount % f == 0)
            {
                factor = f;
                break;
            }
        }
        if (factor == 1)
        {
            skip("no unroll factor up to " + std::to_string(options.maxFactor) +
                 " divides the trip count of " + std::to_string(report.tripCount));
            continue;
        }

        // Give each copy but the last its own registers for iteration-local values.
        const std::bitset<32> local = iterationLocalRegisters(model, block);
        std::vector<std::map<unsigned, unsigned>> renames(static_cast<size_t>(factor));
        size_t nextFree = 0;
        for (int copy = 0; copy + 1 < factor; ++copy)
        {
            for (unsigned r = 1; r < 32 && nextFree < freeRegisters.size(); ++r)
            {
                if (local[r])
                {
                    renames[static_cast<size_t>(copy)][r] = freeRegisters[nextFree];
                    report.renamedTo.push_back(freeRegisters[nextFree]);
                    ++nextFree;
                }
            }
        }

        // The loop's lines, written out once per copy. Only the first copy keeps a label that sits
        // on the first instruction's line, and only the last copy keeps the branch.
        const unsigned branchLine = report.lastLine;
        std::vector<std::string> lines;
        for (int copy = 0; copy < factor; ++copy)
        {
            const bool lastCopy = copy + 1 == factor;
            for (unsigned line = report.firstLine; line <= report.lastLine; ++line)
            {
                if (line == branchLine && !lastCopy)
                {
                    continue;
                }
                const SplitLine split = splitLine(text.lines.at(line - 1));
                std::string prefix = split.prefix;
                if (copy > 0 && line == report.firstLine)
                {
                    prefix = leadingWhitespace(split.prefix);
                    lines.push_back(prefix + "# unrolled iteration " + std::to_string(copy + 1) +
                                    " of " + std::to_string(factor));
                }
                lines.push_back(prefix +
                                renameRegisters(split.body, renames[static_cast<size_t>(copy)]));
            }
        }

        replacements[report.firstLine] = lines;
        for (unsigned line = report.firstLine + 1; line <= report.lastLine; ++line)
        {
            replacements[line] = {};
        }

        report.unrolled = true;
        report.factor = factor;
        result.loops.push_back(report);
        result.changed = true;
    }

    if (!result.changed)
    {
        result.source = source;
        return result;
    }

    SourceText rewritten;
    rewritten.newline = text.newline;
    rewritten.endsWithNewline = text.endsWithNewline;
    for (size_t i = 0; i < text.lines.size(); ++i)
    {
        const auto replacement = replacements.find(static_cast<unsigned>(i + 1));
        if (replacement == replacements.end())
        {
            rewritten.lines.push_back(text.lines[i]);
        }
        else
        {
            rewritten.lines.insert(rewritten.lines.end(), replacement->second.begin(),
                                   replacement->second.end());
        }
    }
    result.source = joinSource(rewritten);
    checkAssembles(result.source, "the unrolled program does not assemble");
    return result;
}

} // namespace optimizer
} // namespace Kites
