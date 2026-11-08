/**
 * @file address_folding.cpp
 * @brief Implementation of the removal of repeated address arithmetic.
 */
#include "optimizer/address_folding.h"

#include <array>
#include <optional>
#include <tuple>

namespace Kites
{
namespace optimizer
{
namespace
{
constexpr int64_t kMinImmediate = -2048;
constexpr int64_t kMaxImmediate = 2047;

// A register's value: an expression plus a constant, computed modulo 2^64 like the hardware.
struct Value
{
    int expression{0};
    uint64_t offset{0};

    bool operator==(const Value &other) const = default;
};

enum class ExpressionKind : uint8_t
{
    Zero,
    Entry,      // a register's value when the code starts
    ShiftLeft,  // a << amount
    Sum,        // a + b
    Difference, // a - b
    Opaque      // made by instruction `a`
};

// Hash-consed expressions, so equal expressions have equal ids.
class Expressions
{
  public:
    Expressions()
    {
        (void)intern(ExpressionKind::Zero, 0, 0); // id 0
    }

    static constexpr int kZero = 0;

    int entry(unsigned reg)
    {
        return reg == 0 ? kZero : intern(ExpressionKind::Entry, reg, 0);
    }

    int shiftLeft(int a, int64_t amount)
    {
        return a == kZero ? kZero : intern(ExpressionKind::ShiftLeft, a, amount);
    }

    int sum(int a, int b)
    {
        if (a == kZero)
        {
            return b;
        }
        if (b == kZero)
        {
            return a;
        }
        return intern(ExpressionKind::Sum, std::min(a, b), std::max(a, b));
    }

    int difference(int a, int b)
    {
        return b == kZero ? a : intern(ExpressionKind::Difference, a, b);
    }

    int opaque(size_t instruction)
    {
        return intern(ExpressionKind::Opaque, static_cast<int64_t>(instruction), 0);
    }

  private:
    int intern(ExpressionKind kind, int64_t a, int64_t b)
    {
        const auto key = std::make_tuple(kind, a, b);
        const auto found = m_ids.find(key);
        if (found != m_ids.end())
        {
            return found->second;
        }
        const int id = static_cast<int>(m_ids.size());
        m_ids.emplace(key, id);
        return id;
    }

    std::map<std::tuple<ExpressionKind, int64_t, int64_t>, int> m_ids;
};

using Registers = std::array<Value, 32>;

// What an instruction must keep doing for the code to stay equivalent.
struct Observation
{
    Value result;  // arithmetic: the value written
    Value address; // loads and stores
    Value data;    // stores
    std::vector<Value> reads; // branches and Other: every value read
};

struct Trace
{
    std::vector<Observation> observations;
    Registers atEnd{};
};

Registers entryRegisters(Expressions &expressions)
{
    Registers registers{};
    for (unsigned r = 0; r < 32; ++r)
    {
        registers[r] = {expressions.entry(r), 0};
    }
    return registers;
}

void write(Registers &registers, unsigned rd, Value value)
{
    if (rd != 0)
    {
        registers[rd] = value;
    }
}

bool isArithmetic(FoldKind kind)
{
    return kind == FoldKind::AddImmediate || kind == FoldKind::ShiftLeftImmediate ||
           kind == FoldKind::Add || kind == FoldKind::Subtract;
}

Value arithmetic(const FoldInstr &instr, const Registers &registers, Expressions &expressions)
{
    const Value a = registers[instr.rs1];
    const Value b = registers[instr.rs2];
    switch (instr.kind)
    {
    case FoldKind::AddImmediate:
        return {a.expression, a.offset + static_cast<uint64_t>(instr.imm)};
    case FoldKind::ShiftLeftImmediate:
        return {expressions.shiftLeft(a.expression, instr.imm), a.offset << instr.imm};
    case FoldKind::Add:
        return {expressions.sum(a.expression, b.expression), a.offset + b.offset};
    default: // Subtract
        return {expressions.difference(a.expression, b.expression), a.offset - b.offset};
    }
}

std::vector<Value> readsOf(const FoldInstr &instr, const Registers &registers)
{
    std::vector<Value> reads;
    if (instr.kind == FoldKind::Branch)
    {
        reads = {registers[instr.rs1], registers[instr.rs2]};
    }
    else
    {
        for (unsigned reg : instr.sources)
        {
            reads.push_back(registers[reg]);
        }
    }
    return reads;
}

// The original code's behaviour, which every rewrite must reproduce.
Trace traceOf(const std::vector<FoldInstr> &code, Expressions &expressions)
{
    Trace trace;
    Registers registers = entryRegisters(expressions);
    trace.observations.resize(code.size());
    for (size_t j = 0; j < code.size(); ++j)
    {
        const FoldInstr &instr = code[j];
        Observation &seen = trace.observations[j];
        if (isArithmetic(instr.kind))
        {
            seen.result = arithmetic(instr, registers, expressions);
            write(registers, instr.rd, seen.result);
            continue;
        }
        switch (instr.kind)
        {
        case FoldKind::Load:
            seen.address = {registers[instr.rs1].expression,
                            registers[instr.rs1].offset + static_cast<uint64_t>(instr.imm)};
            write(registers, instr.rd, {expressions.opaque(j), 0});
            break;
        case FoldKind::Store:
            seen.address = {registers[instr.rs1].expression,
                            registers[instr.rs1].offset + static_cast<uint64_t>(instr.imm)};
            seen.data = registers[instr.rs2];
            break;
        default: // Branch, Other
            seen.reads = readsOf(instr, registers);
            write(registers, instr.rd, {expressions.opaque(j), 0});
            break;
        }
    }
    trace.atEnd = registers;
    return trace;
}

// Points `instr` (an addi, load or store) at a register holding `wanted`'s expression, adjusting
// the immediate so it computes `wanted` exactly. Prefers the register it already uses.
bool retarget(FoldInstr &instr, const Registers &registers, Value wanted)
{
    if (!instr.rewritable)
    {
        return false;
    }
    auto tryRegister = [&](unsigned reg)
    {
        if (registers[reg].expression != wanted.expression)
        {
            return false;
        }
        const auto difference = static_cast<int64_t>(wanted.offset - registers[reg].offset);
        if (difference < kMinImmediate || difference > kMaxImmediate)
        {
            return false;
        }
        if (reg != instr.rs1 || difference != instr.imm)
        {
            instr.rs1 = reg;
            instr.imm = difference;
            instr.rewritten = true;
        }
        return true;
    };
    if (tryRegister(instr.rs1))
    {
        return true;
    }
    for (unsigned reg = 0; reg < 32; ++reg)
    {
        if (tryRegister(reg))
        {
            return true;
        }
    }
    return false;
}

// Runs `code` (with its removals and rewrites), repairing what can be repaired. Returns the final
// registers if the code still does exactly what `reference` records.
std::optional<Registers> replay(std::vector<FoldInstr> &code, const Trace &reference,
                                Expressions &expressions)
{
    Registers registers = entryRegisters(expressions);
    for (size_t j = 0; j < code.size(); ++j)
    {
        FoldInstr &instr = code[j];
        if (instr.removed)
        {
            continue;
        }
        const Observation &wanted = reference.observations[j];
        if (isArithmetic(instr.kind))
        {
            if (arithmetic(instr, registers, expressions) != wanted.result &&
                (instr.kind != FoldKind::AddImmediate || !retarget(instr, registers, wanted.result)))
            {
                return std::nullopt;
            }
            write(registers, instr.rd, wanted.result);
            continue;
        }
        switch (instr.kind)
        {
        case FoldKind::Load:
        case FoldKind::Store:
        {
            const Value address{registers[instr.rs1].expression,
                                registers[instr.rs1].offset + static_cast<uint64_t>(instr.imm)};
            if (address != wanted.address && !retarget(instr, registers, wanted.address))
            {
                return std::nullopt;
            }
            if (instr.kind == FoldKind::Store && registers[instr.rs2] != wanted.data)
            {
                return std::nullopt;
            }
            if (instr.kind == FoldKind::Load)
            {
                write(registers, instr.rd, {expressions.opaque(j), 0});
            }
            break;
        }
        default:
            if (readsOf(instr, registers) != wanted.reads)
            {
                return std::nullopt;
            }
            write(registers, instr.rd, {expressions.opaque(j), 0});
            break;
        }
    }
    return registers;
}

const char *loadMnemonic(unsigned funct3)
{
    static const char *const names[] = {"lb", "lh", "lw", "ld", "lbu", "lhu", "lwu", "?"};
    return names[funct3 & 7];
}

const char *storeMnemonic(unsigned funct3)
{
    static const char *const names[] = {"sb", "sh", "sw", "sd", "?", "?", "?", "?"};
    return names[funct3 & 7];
}
} // namespace

FoldInstr makeFoldInstr(const InstrInfo &info, const std::map<unsigned, unsigned> &renames,
                        bool rewritable)
{
    auto renamed = [&](unsigned reg)
    {
        const auto found = renames.find(reg);
        return found == renames.end() ? reg : found->second;
    };
    const uint32_t word = info.word;
    const unsigned opcode = word & 0x7F;
    const unsigned funct3 = (word >> 12) & 0x7;
    const unsigned funct7 = word >> 25;

    FoldInstr instr;
    instr.rewritable = rewritable;
    instr.rd = renamed((word >> 7) & 0x1F);
    instr.rs1 = renamed((word >> 15) & 0x1F);
    instr.rs2 = renamed((word >> 20) & 0x1F);
    instr.imm = info.imm;
    instr.funct3 = funct3;

    if (opcode == 0b0010011 && funct3 == 0)
    {
        instr.kind = FoldKind::AddImmediate;
    }
    else if (opcode == 0b0010011 && funct3 == 1 && (word >> 26) == 0)
    {
        instr.kind = FoldKind::ShiftLeftImmediate;
        instr.imm = (word >> 20) & 0x3F;
    }
    else if (opcode == 0b0110011 && funct3 == 0 && funct7 == 0)
    {
        instr.kind = FoldKind::Add;
    }
    else if (opcode == 0b0110011 && funct3 == 0 && funct7 == 0x20)
    {
        instr.kind = FoldKind::Subtract;
    }
    else if (opcode == 0b0000011)
    {
        instr.kind = FoldKind::Load;
    }
    else if (opcode == 0b0100011)
    {
        instr.kind = FoldKind::Store;
        instr.rd = 0; // those bits hold part of the offset
    }
    else if (info.klass == InstrClass::Branch)
    {
        instr.kind = FoldKind::Branch;
        instr.rd = 0;
    }
    else
    {
        // Anything else: read what it reads, and write an opaque value.
        instr.kind = FoldKind::Other;
        for (size_t s = 0; s < info.sourceCount; ++s)
        {
            if (info.sources[s].file == RegFile::Gpr)
            {
                instr.sources.push_back(renamed(info.sources[s].index));
            }
        }
        instr.rd = info.dest.isValid() && info.dest.file == RegFile::Gpr
                       ? renamed(info.dest.index)
                       : 0;
    }
    return instr;
}

std::string foldedAssembly(const FoldInstr &instr)
{
    auto reg = [](unsigned r) { return "x" + std::to_string(r); };
    switch (instr.kind)
    {
    case FoldKind::AddImmediate:
        return "addi " + reg(instr.rd) + ", " + reg(instr.rs1) + ", " + std::to_string(instr.imm);
    case FoldKind::Load:
        return std::string(loadMnemonic(instr.funct3)) + " " + reg(instr.rd) + ", " +
               std::to_string(instr.imm) + "(" + reg(instr.rs1) + ")";
    case FoldKind::Store:
        return std::string(storeMnemonic(instr.funct3)) + " " + reg(instr.rs2) + ", " +
               std::to_string(instr.imm) + "(" + reg(instr.rs1) + ")";
    default:
        return {};
    }
}

FoldResult foldAddresses(std::vector<FoldInstr> &code, const std::bitset<32> &keepAtEnd)
{
    Expressions expressions;
    const Trace reference = traceOf(code, expressions);

    FoldResult result;
    // Later instructions first, so a whole address chain goes: its readers are removed before
    // the instructions they read from are tried. Repeat until nothing more can go.
    for (bool progress = true; progress;)
    {
        progress = false;
        for (size_t c = code.size(); c-- > 0;)
        {
            const FoldInstr &candidate = code[c];
            if (candidate.removed || !candidate.rewritable || !isArithmetic(candidate.kind) ||
                candidate.rd == 0)
            {
                continue;
            }
            std::vector<FoldInstr> trial = code;
            trial[c].removed = true;
            const std::optional<Registers> atEnd = replay(trial, reference, expressions);
            if (!atEnd)
            {
                continue;
            }
            bool keeps = true;
            for (unsigned r = 1; r < 32 && keeps; ++r)
            {
                keeps = !keepAtEnd[r] || (*atEnd)[r] == reference.atEnd[r];
            }
            if (!keeps)
            {
                continue;
            }
            code = std::move(trial);
            ++result.removed;
            progress = true;
        }
    }

    if (result.removed > 0)
    {
        const std::optional<Registers> atEnd = replay(code, reference, expressions);
        for (unsigned r = 1; r < 32; ++r)
        {
            result.changedAtEnd[r] = (*atEnd)[r] != reference.atEnd[r];
        }
    }
    return result;
}

} // namespace optimizer
} // namespace Kites
