#include "processor/syscall_handler.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Kites
{
namespace
{
constexpr size_t kA0 = 10;
constexpr size_t kA1 = 11;
constexpr size_t kA2 = 12;
constexpr size_t kA7 = 17;
constexpr size_t kFa0 = 10;

// A string longer than this is taken to be missing its terminator.
constexpr size_t kMaxStringLength = 1u << 20;

enum SyscallNumber : uint64_t
{
    PrintInt = 1,
    PrintFloat = 2,
    PrintDouble = 3,
    PrintString = 4,
    ReadInt = 5,
    ReadFloat = 6,
    ReadDouble = 7,
    ReadString = 8,
    Exit = 10,
    PrintChar = 11,
    ReadChar = 12,
    Read = 63,
    Write = 64,
    ExitWithCode = 93,
};

// The shortest text that reads back as exactly the same number.
template <typename T> std::string shortestText(T value, int maxDigits)
{
    if (std::isnan(value))
    {
        return "NaN";
    }
    if (std::isinf(value))
    {
        return value < 0 ? "-Infinity" : "Infinity";
    }
    char buffer[64];
    for (int digits = 1; digits <= maxDigits; ++digits)
    {
        std::snprintf(buffer, sizeof buffer, "%.*g", digits, static_cast<double>(value));
        const double back = std::strtod(buffer, nullptr);
        if (static_cast<T>(back) == value)
        {
            break;
        }
    }
    return buffer;
}

float floatFromBits(uint64_t bits)
{
    const uint32_t low = static_cast<uint32_t>(bits);
    float value;
    std::memcpy(&value, &low, sizeof value);
    return value;
}

double doubleFromBits(uint64_t bits)
{
    double value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

uint64_t bitsOfFloat(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

uint64_t bitsOfDouble(double value)
{
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

void setGpr(RegisterFile &registers, SyscallEffect &effect, size_t index, uint64_t value)
{
    const uint64_t old = registers.ReadGpr(index);
    registers.WriteGpr(index, value);
    if (old != value)
    {
        effect.registerChanges.push_back({static_cast<unsigned>(index), 0, old, value});
    }
}

void setFpr(RegisterFile &registers, SyscallEffect &effect, size_t index, uint64_t value)
{
    const uint64_t old = registers.ReadFpr(index);
    registers.WriteFpr(index, value);
    if (old != value)
    {
        effect.registerChanges.push_back({static_cast<unsigned>(index), 2, old, value});
    }
}

// Stores `bytes` at `address` and records the old and new contents.
void storeBytes(MemoryController &memory, SyscallEffect &effect, uint64_t address,
                const std::string &bytes)
{
    if (bytes.empty())
    {
        return;
    }
    MemoryChange change;
    change.address = address;
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        change.old_bytes_vec.push_back(memory.peekByte(address + i));
    }
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        memory.writeByte(address + i, static_cast<uint8_t>(bytes[i]));
        change.new_bytes_vec.push_back(static_cast<uint8_t>(bytes[i]));
    }
    effect.memoryChanges.push_back(std::move(change));
}

std::string loadString(MemoryController &memory, uint64_t address)
{
    std::string text;
    while (text.size() < kMaxStringLength)
    {
        const char c = static_cast<char>(memory.readByte(address + text.size()));
        if (c == '\0')
        {
            break;
        }
        text.push_back(c);
    }
    return text;
}

std::string loadBytes(MemoryController &memory, uint64_t address, uint64_t count)
{
    std::string text;
    for (uint64_t i = 0; i < count && i < kMaxStringLength; ++i)
    {
        text.push_back(static_cast<char>(memory.readByte(address + i)));
    }
    return text;
}

void print(IoChannel &io, SyscallEffect &effect, const std::string &text)
{
    io.write(text);
    effect.io = IoEvent{IoEvent::Kind::Output, text};
}

// Reads a number; text that is not a number reads as 0.
template <typename T, typename Parse> T parseOrZero(const std::string &token, Parse parse)
{
    try
    {
        return parse(token);
    }
    catch (const std::exception &)
    {
        return T{};
    }
}
} // namespace

bool SyscallHandler::wouldWait(const RegisterFile &registers, const IoChannel &io)
{
    if (io.usesStdio())
    {
        return false; // the terminal can always be waited on, and ends with EOF
    }
    switch (registers.ReadGpr(kA7))
    {
    case ReadInt:
    case ReadFloat:
    case ReadDouble:
        return !io.hasToken();
    case ReadChar:
        return !io.hasChar();
    case ReadString:
        return !io.hasLine();
    case Read:
        return registers.ReadGpr(kA0) == 0 && !io.hasLine();
    default:
        return false;
    }
}

SyscallEffect SyscallHandler::execute(RegisterFile &registers, MemoryController &memory,
                                      IoChannel &io, const std::atomic<bool> *cancel)
{
    SyscallEffect effect;
    const uint64_t number = registers.ReadGpr(kA7);
    const uint64_t a0 = registers.ReadGpr(kA0);
    const uint64_t a1 = registers.ReadGpr(kA1);
    const uint64_t a2 = registers.ReadGpr(kA2);

    // Reads share their handling of the outcome.
    auto failed = [&](ReadStatus status)
    {
        if (status == ReadStatus::Cancelled || status == ReadStatus::WouldWait)
        {
            effect.cancelled = true;
        }
    };
    auto consumed = [&](const std::string &text)
    { effect.io = IoEvent{IoEvent::Kind::Input, text}; };

    switch (number)
    {
    case PrintInt:
        print(io, effect, std::to_string(static_cast<int64_t>(a0)));
        break;
    case PrintFloat:
        print(io, effect, shortestText(floatFromBits(registers.ReadFpr(kFa0)), 9));
        break;
    case PrintDouble:
        print(io, effect, shortestText(doubleFromBits(registers.ReadFpr(kFa0)), 17));
        break;
    case PrintString:
        print(io, effect, loadString(memory, a0));
        break;
    case PrintChar:
        print(io, effect, std::string(1, static_cast<char>(a0)));
        break;

    case ReadInt:
    case ReadFloat:
    case ReadDouble:
    {
        std::string token;
        std::string text;
        const ReadStatus status = io.readToken(token, text, cancel);
        if (status == ReadStatus::Ok)
        {
            consumed(text);
        }
        else if (status != ReadStatus::Eof)
        {
            failed(status);
            break;
        }
        if (number == ReadInt)
        {
            setGpr(registers, effect, kA0,
                   static_cast<uint64_t>(parseOrZero<int64_t>(
                       token, [](const std::string &t) { return std::stoll(t); })));
        }
        else if (number == ReadFloat)
        {
            setFpr(registers, effect, kFa0,
                   bitsOfFloat(parseOrZero<float>(
                       token, [](const std::string &t) { return std::stof(t); })));
        }
        else
        {
            setFpr(registers, effect, kFa0,
                   bitsOfDouble(parseOrZero<double>(
                       token, [](const std::string &t) { return std::stod(t); })));
        }
        break;
    }
    case ReadChar:
    {
        char c = 0;
        const ReadStatus status = io.readChar(c, cancel);
        if (status == ReadStatus::Ok)
        {
            consumed(std::string(1, c));
            setGpr(registers, effect, kA0, static_cast<uint8_t>(c));
        }
        else if (status == ReadStatus::Eof)
        {
            setGpr(registers, effect, kA0, static_cast<uint64_t>(-1));
        }
        else
        {
            failed(status);
        }
        break;
    }
    case ReadString:
    case Read:
    {
        if (number == Read && a0 != 0)
        {
            setGpr(registers, effect, kA0, static_cast<uint64_t>(-1)); // only stdin can be read
            break;
        }
        const uint64_t buffer = number == Read ? a1 : a0;
        const uint64_t size = number == Read ? a2 : a1;
        // read_string leaves room for the NUL; read(2) fills the buffer.
        const uint64_t room = number == ReadString ? (size > 0 ? size - 1 : 0) : size;
        std::string line;
        const ReadStatus status = io.readLine(line, static_cast<size_t>(room), cancel);
        if (status != ReadStatus::Ok && status != ReadStatus::Eof)
        {
            failed(status);
            break;
        }
        if (status == ReadStatus::Ok)
        {
            consumed(line);
        }
        std::string stored = line;
        if (size > line.size())
        {
            stored.push_back('\0');
        }
        storeBytes(memory, effect, buffer, stored);
        setGpr(registers, effect, kA0, line.size());
        break;
    }

    case Write:
        if (a0 == 1 || a0 == 2)
        {
            print(io, effect, loadBytes(memory, a1, a2));
            setGpr(registers, effect, kA0, a2);
        }
        else
        {
            setGpr(registers, effect, kA0, static_cast<uint64_t>(-1));
        }
        break;

    case Exit:
        effect.exited = true;
        effect.exitCode = 0;
        break;
    case ExitWithCode:
        effect.exited = true;
        effect.exitCode = static_cast<int>(a0);
        break;

    default:
        effect.unknown = true;
        break;
    }
    return effect;
}

} // namespace Kites
