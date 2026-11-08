/**
 * @file loop_unroller.cpp
 * @brief Implementation of the exact loop unroller.
 */
#include "optimizer/loop_unroller.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <map>
#include <sstream>
#include <stdexcept>

#include "assembler/assembler.h"
#include "optimizer/address_folding.h"
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

// The order spare registers are handed out in, following the RISC-V calling convention: the
// temporaries first (t3-t6, t0-t2), then the argument registers (a7-a0), and the callee-saved
// registers (s11-s1, then s0, the frame pointer) only when nothing else is free, since code inside a
// function would have to save those. x0-x4 (zero, ra, sp, gp, tp) are never handed out.
constexpr unsigned kAllocationOrder[] = {31, 30, 29, 28, 7,  6,  5,  17, 16, 15, 14, 13, 12, 11,
                                         10, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 9,  8};

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

// `body` (an instruction and maybe a comment) with the instruction replaced, keeping the comment
// where it was.
std::string withInstruction(const std::string &body, const std::string &instruction)
{
    // Keep the operands in the column the original put them in ("lw   x5, 0(x10)").
    std::string text = instruction;
    const size_t mnemonicEnd = body.find_first_of(" \t");
    const size_t operandsStart = mnemonicEnd == std::string::npos
                                     ? std::string::npos
                                     : body.find_first_not_of(" \t", mnemonicEnd);
    const size_t space = instruction.find(' ');
    if (operandsStart != std::string::npos && space != std::string::npos && space < operandsStart)
    {
        text = instruction.substr(0, space) + std::string(operandsStart - space, ' ') +
               instruction.substr(space + 1);
    }

    const size_t comment = body.find('#');
    if (comment == std::string::npos)
    {
        return text;
    }
    text.resize(std::max(text.size() + 1, comment), ' ');
    return text + body.substr(comment);
}
} // namespace

bool isCalleeSaved(unsigned reg)
{
    return reg == 8 || reg == 9 || (reg >= 18 && reg <= 27);
}

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
    const std::vector<BlockLiveness> liveness = computeLiveness(model);

    std::vector<unsigned> freeRegisters;
    for (unsigned r : kAllocationOrder)
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

        // The copies as straight-line code (the branch only in the last copy), so that the
        // address arithmetic they repeat can be folded away.
        const size_t branchUnit = block.firstUnit + block.unitCount - 1;
        std::vector<FoldInstr> code;
        std::vector<std::map<size_t, size_t>> codeOfUnit(static_cast<size_t>(factor));
        std::map<unsigned, size_t> unitOfLine;
        for (int copy = 0; copy < factor; ++copy)
        {
            for (size_t u = block.firstUnit; u < block.firstUnit + block.unitCount; ++u)
            {
                const Unit &unit = model.units[u];
                unitOfLine[unit.sourceLine] = u;
                if (u == branchUnit && copy + 1 < factor)
                {
                    continue;
                }
                codeOfUnit[static_cast<size_t>(copy)][u] = code.size();
                for (size_t k = 0; k < unit.instructionCount; ++k)
                {
                    code.push_back(makeFoldInstr(model.instructions[unit.firstInstruction + k],
                                                 renames[static_cast<size_t>(copy)],
                                                 unit.instructionCount == 1));
                }
            }
        }
        if (options.foldAddresses)
        {
            // Registers whose final value must not change: all of them, or only those read after
            // the loop (which includes everything the loop itself carries into its next round).
            std::bitset<32> keep;
            if (options.allowDeadRegisterChanges)
            {
                for (unsigned r = 0; r < 32; ++r)
                {
                    keep[r] = liveness[b].liveOut[r];
                }
            }
            else
            {
                keep.set();
            }
            for (unsigned r : report.renamedTo)
            {
                keep.reset(r); // nobody else reads the copies' own registers
            }
            const FoldResult folded = foldAddresses(code, keep);
            report.foldedInstructions = folded.removed;
            for (unsigned r = 1; r < 32; ++r)
            {
                if (folded.changedAtEnd[r] &&
                    std::find(report.renamedTo.begin(), report.renamedTo.end(), r) ==
                        report.renamedTo.end())
                {
                    report.changedDeadRegisters.push_back(r);
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
                if (line == report.firstLine)
                {
                    // One note at the top. Markers between the copies would end up in the middle
                    // of interleaved code once the scheduler reorders the body.
                    if (copy == 0)
                    {
                        lines.push_back(leadingWhitespace(split.prefix) + "# loop unrolled x" +
                                        std::to_string(factor));
                    }
                    else
                    {
                        prefix = leadingWhitespace(split.prefix);
                    }
                }
                std::string body = renameRegisters(split.body, renames[static_cast<size_t>(copy)]);
                const auto unit = unitOfLine.find(line);
                if (unit != unitOfLine.end() && model.units[unit->second].instructionCount == 1)
                {
                    const FoldInstr &instr =
                        code[codeOfUnit[static_cast<size_t>(copy)].at(unit->second)];
                    if (instr.removed)
                    {
                        if (hasLabel(prefix))
                        {
                            lines.push_back(rightTrimmed(prefix)); // the label stays
                        }
                        continue;
                    }
                    if (instr.rewritten)
                    {
                        body = withInstruction(body, foldedAssembly(instr));
                    }
                }
                lines.push_back(prefix + body);
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

namespace
{
// A way a loop computes an address from its counter: base + (counter << shift). `base` is 0 when
// there is no base register.
struct PointerCandidate
{
    unsigned counter{0};
    int64_t step{0};
    unsigned shift{0};
    unsigned base{0};

    bool operator==(const PointerCandidate &other) const = default;
};

bool isSelfLoop(const ProgramModel &model, const BasicBlock &block)
{
    const InstrInfo &last = model.instructions[block.firstInstruction + block.instructionCount - 1];
    return last.klass == InstrClass::Branch && last.hasTarget &&
           last.targetPc == model.instructions[block.firstInstruction].pc;
}

// Tries one pointer in one loop. On success rewrites `source` and fills `report`.
bool tryPointer(const ProgramModel &model, const std::vector<BlockLiveness> &liveness, size_t b,
                const PointerCandidate &candidate, unsigned pointer,
                const std::vector<unsigned> &notKept, bool allowDeadRegisterChanges,
                std::string &source, PointerReport &report)
{
    const BasicBlock &block = model.blocks[b];
    const int64_t stride = static_cast<int64_t>(static_cast<uint64_t>(candidate.step)
                                                << candidate.shift);
    if (stride < -2048 || stride > 2047)
    {
        return false;
    }

    // Straight-line code for one round: the set-up before the loop, the body, the step, the
    // branch. The added instructions are never removed or rewritten.
    std::vector<FoldInstr> code;
    auto added = [](FoldKind kind, unsigned rd, unsigned rs1, unsigned rs2, int64_t imm)
    {
        FoldInstr instr;
        instr.kind = kind;
        instr.rd = rd;
        instr.rs1 = rs1;
        instr.rs2 = rs2;
        instr.imm = imm;
        instr.rewritable = false;
        return instr;
    };
    code.push_back(added(FoldKind::ShiftLeftImmediate, pointer, candidate.counter, 0,
                         candidate.shift));
    if (candidate.base != 0)
    {
        code.push_back(added(FoldKind::Add, pointer, candidate.base, pointer, 0));
    }
    const size_t setupCount = code.size();

    // Where the pointer steps: right after the last load, so the loads use it before the step and
    // the stores after it (with offsets one stride lower), and the next round's first loads are
    // as far from the step as possible. Without loads, just before the branch.
    const size_t branchUnit = block.firstUnit + block.unitCount - 1;
    size_t stepAfterUnit = branchUnit; // "before the branch"
    for (size_t u = block.firstUnit; u < branchUnit; ++u)
    {
        const Unit &unit = model.units[u];
        for (size_t k = 0; k < unit.instructionCount; ++k)
        {
            if (model.instructions[unit.firstInstruction + k].klass == InstrClass::Load)
            {
                stepAfterUnit = u;
            }
        }
    }
    std::map<size_t, size_t> codeOfUnit;
    for (size_t u = block.firstUnit; u < block.firstUnit + block.unitCount; ++u)
    {
        const Unit &unit = model.units[u];
        if (u == branchUnit && stepAfterUnit == branchUnit)
        {
            code.push_back(added(FoldKind::AddImmediate, pointer, pointer, 0, stride));
        }
        codeOfUnit[u] = code.size();
        for (size_t k = 0; k < unit.instructionCount; ++k)
        {
            code.push_back(makeFoldInstr(model.instructions[unit.firstInstruction + k], {},
                                         unit.instructionCount == 1));
        }
        if (u == stepAfterUnit && u != branchUnit)
        {
            code.push_back(added(FoldKind::AddImmediate, pointer, pointer, 0, stride));
        }
    }

    std::bitset<32> keep;
    if (allowDeadRegisterChanges)
    {
        for (unsigned r = 0; r < 32; ++r)
        {
            keep[r] = liveness[b].liveOut[r];
        }
    }
    else
    {
        keep.set();
    }
    for (unsigned r : notKept)
    {
        keep.reset(r);
    }
    keep.set(pointer); // carried into the next round
    keep.set(candidate.counter);

    const FoldResult folded = foldAddresses(code, keep);
    if (folded.removed < 2) // the loop gained a step, so it must lose at least two
    {
        return false;
    }

    // Write it out: the set-up after the block before the loop, the step before the branch.
    const SourceText text = splitSource(source);
    const unsigned firstLine = model.units[block.firstUnit].sourceLine;
    const unsigned lastLine = model.units[branchUnit].sourceLine;
    // The set-up goes right after the last write to the counter or the base in the block before
    // the loop (or at its end if it writes neither). Anything later in that block then stands
    // between the set-up and the loop, so the loop can use the pointer straight away.
    const BasicBlock &before = model.blocks[b - 1];
    unsigned setupAfter = model.units[before.firstUnit + before.unitCount - 1].sourceLine;
    for (size_t u = before.firstUnit; u < before.firstUnit + before.unitCount; ++u)
    {
        const Unit &unit = model.units[u];
        for (size_t k = 0; k < unit.instructionCount; ++k)
        {
            const InstrInfo &info = model.instructions[unit.firstInstruction + k];
            if (info.dest.isValid() && info.dest.file == RegFile::Gpr &&
                (info.dest.index == candidate.counter ||
                 (candidate.base != 0 && info.dest.index == candidate.base)))
            {
                setupAfter = unit.sourceLine;
            }
        }
    }
    if (firstLine == 0 || lastLine == 0 || setupAfter == 0 || setupAfter >= firstLine)
    {
        return false;
    }
    const std::string indent = leadingWhitespace(splitLine(text.lines.at(firstLine - 1)).prefix);
    const std::string branchIndent =
        leadingWhitespace(splitLine(text.lines.at(lastLine - 1)).prefix);
    auto reg = [](unsigned r) { return "x" + std::to_string(r); };

    std::map<unsigned, std::vector<std::string>> replacements;
    std::vector<std::string> setup = {text.lines.at(setupAfter - 1)};
    setup.push_back(indent + "slli " + reg(pointer) + ", " + reg(candidate.counter) + ", " +
                    std::to_string(candidate.shift) + "  # pointer for the loop below");
    if (candidate.base != 0)
    {
        setup.push_back(indent + "add  " + reg(pointer) + ", " + reg(candidate.base) + ", " +
                        reg(pointer));
    }
    replacements[setupAfter] = setup;

    for (size_t u = block.firstUnit; u < block.firstUnit + block.unitCount; ++u)
    {
        const Unit &unit = model.units[u];
        const unsigned line = unit.sourceLine;
        std::vector<std::string> lines;
        const std::string step =
            "addi " + reg(pointer) + ", " + reg(pointer) + ", " + std::to_string(stride);
        if (u == branchUnit && stepAfterUnit == branchUnit)
        {
            lines.push_back(branchIndent + step);
        }
        const SplitLine split = splitLine(text.lines.at(line - 1));
        if (unit.instructionCount == 1)
        {
            const FoldInstr &instr = code[codeOfUnit.at(u)];
            if (instr.removed)
            {
                if (hasLabel(split.prefix))
                {
                    lines.push_back(rightTrimmed(split.prefix));
                }
            }
            else if (instr.rewritten)
            {
                lines.push_back(split.prefix + withInstruction(split.body, foldedAssembly(instr)));
            }
            else
            {
                lines.push_back(text.lines.at(line - 1));
            }
        }
        else
        {
            lines.push_back(text.lines.at(line - 1));
        }
        if (u == stepAfterUnit && u != branchUnit)
        {
            lines.push_back(indent + step);
        }
        replacements[line] = lines;
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
    source = joinSource(rewritten);
    checkAssembles(source, "the program with a loop pointer does not assemble");

    report.firstLine = firstLine;
    report.lastLine = lastLine;
    report.pointer = pointer;
    report.stride = stride;
    report.removed = folded.removed;
    for (unsigned r = 1; r < 32; ++r)
    {
        if (folded.changedAtEnd[r] && r != pointer &&
            std::find(notKept.begin(), notKept.end(), r) == notKept.end())
        {
            report.changedDeadRegisters.push_back(r);
        }
    }
    (void)setupCount;
    return true;
}

// Finds and makes one pointer anywhere in the program. False if there is none to make.
bool introduceOnePointer(PointerResult &result, std::vector<unsigned> &notKept,
                         bool allowDeadRegisterChanges)
{
    std::istringstream stream(result.source);
    const AssembledProgram program = assemble(stream);
    const ProgramModel model = buildProgramModel(program);
    const ControlFlow flow = computeControlFlow(model);
    const std::vector<BlockLiveness> liveness = computeLiveness(model);
    const std::bitset<32> used = registersUsedAnywhere(model);

    unsigned pointer = 0;
    for (unsigned r : kAllocationOrder)
    {
        if (!used[r])
        {
            pointer = r;
            break;
        }
    }
    if (pointer == 0)
    {
        return false;
    }

    for (size_t b = 1; b < model.blocks.size(); ++b)
    {
        const BasicBlock &block = model.blocks[b];
        if (!isSelfLoop(model, block) || !block.isReorderable || flow.hasUnknownPredecessors[b])
        {
            continue;
        }
        // Entered only by falling in from the block before (not by a jump to the label).
        bool onlyFallThrough = true;
        for (size_t predecessor : flow.predecessors[b])
        {
            onlyFallThrough = onlyFallThrough && (predecessor == b || predecessor == b - 1);
        }
        const BasicBlock &before = model.blocks[b - 1];
        const InstrInfo &beforeLast =
            model.instructions[before.firstInstruction + before.instructionCount - 1];
        const uint64_t loopStart = model.instructions[block.firstInstruction].pc;
        if (!onlyFallThrough || beforeLast.klass == InstrClass::Jal ||
            beforeLast.klass == InstrClass::Jalr ||
            (beforeLast.klass == InstrClass::Branch && beforeLast.hasTarget &&
             beforeLast.targetPc == loopStart))
        {
            continue;
        }

        std::vector<FoldInstr> body;
        bool floatingPoint = false;
        for (size_t k = block.firstInstruction; k < block.firstInstruction + block.instructionCount;
             ++k)
        {
            floatingPoint = floatingPoint || usesFloatingPoint(model.instructions[k]);
            body.push_back(makeFoldInstr(model.instructions[k], {}, true));
        }
        if (floatingPoint)
        {
            continue;
        }
        std::array<int, 32> writes{};
        std::array<const FoldInstr *, 32> writer{};
        for (const FoldInstr &instr : body)
        {
            if (instr.rd != 0)
            {
                ++writes[instr.rd];
                writer[instr.rd] = &instr;
            }
        }
        auto isCounter = [&](unsigned r)
        {
            return r != 0 && writes[r] == 1 && writer[r]->kind == FoldKind::AddImmediate &&
                   writer[r]->rs1 == r && writer[r]->imm != 0;
        };

        std::vector<PointerCandidate> candidates;
        for (size_t i = 0; i < body.size(); ++i)
        {
            const FoldInstr &shift = body[i];
            if (shift.kind != FoldKind::ShiftLeftImmediate || !isCounter(shift.rs1) ||
                shift.rd == 0)
            {
                continue;
            }
            PointerCandidate candidate{shift.rs1, writer[shift.rs1]->imm,
                                       static_cast<unsigned>(shift.imm), 0};
            for (size_t j = i + 1; j < body.size(); ++j)
            {
                const FoldInstr &next = body[j];
                if (next.kind == FoldKind::Add && (next.rs1 == shift.rd || next.rs2 == shift.rd))
                {
                    const unsigned other = next.rs1 == shift.rd ? next.rs2 : next.rs1;
                    if (other != 0 && writes[other] == 0)
                    {
                        candidate.base = other;
                    }
                    break;
                }
                if (next.rd == shift.rd)
                {
                    break;
                }
            }
            if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
            {
                candidates.push_back(candidate);
            }
        }

        for (const PointerCandidate &candidate : candidates)
        {
            PointerReport report;
            if (tryPointer(model, liveness, b, candidate, pointer, notKept,
                           allowDeadRegisterChanges, result.source, report))
            {
                notKept.push_back(pointer);
                result.pointers.push_back(report);
                result.changed = true;
                return true;
            }
        }
    }
    return false;
}
} // namespace

PointerResult introducePointers(const std::string &source, const PointerOptions &options)
{
    {
        std::istringstream stream(source);
        try
        {
            (void)assemble(stream);
        }
        catch (const std::exception &error)
        {
            throw std::runtime_error(std::string("the program does not assemble: ") + error.what());
        }
    }
    PointerResult result;
    result.source = source;
    std::vector<unsigned> notKept = options.optimizerRegisters;
    // One pointer at a time, re-reading the program after each, so later ones see earlier ones.
    for (int round = 0; round < 16; ++round)
    {
        if (!introduceOnePointer(result, notKept, options.allowDeadRegisterChanges))
        {
            break;
        }
    }
    return result;
}

} // namespace optimizer
} // namespace Kites
