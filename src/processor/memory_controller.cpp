#include "memory_controller.h"

namespace Kites
{
    
MemoryController::MemoryController() : 
l2_cache_(static_cast<MemoryDevice&>(memory_)), 
l1_cache_(static_cast<MemoryDevice&>(l2_cache_)), 
instruction_cache_(static_cast<MemoryDevice&>(l2_cache_)) // casting needed here otherwise
{}                                                        // compiler think we are calling 
                                                          // copy constructor


void MemoryController::setIoChannel(IoChannel *io)
{
    console_ = io ? std::make_unique<ConsoleDevice>(io, &ioEvents_) : nullptr;
}

std::vector<IoEvent> MemoryController::takeIoEvents()
{
    return std::exchange(ioEvents_, {});
}

void MemoryController::restoreByte(uint64_t address, uint8_t value)
{
    if (!isDevice(address))
    {
        writeByte(address, value);
    }
}

// Device registers are 32 bits wide. A narrower access sees the part of the register it names,
// and a wider one is several registers side by side.
uint64_t MemoryController::deviceRead(uint64_t address, unsigned size)
{
    if (!console_)
    {
        return 0;
    }
    const uint32_t offset = static_cast<uint32_t>(address - kConsoleBase);
    if (size == 8)
    {
        return console_->read(offset) | (static_cast<uint64_t>(console_->read(offset + 4)) << 32);
    }
    const uint64_t word = console_->read(offset & ~3u);
    const uint64_t shifted = word >> ((offset & 3u) * 8);
    return size == 4 ? shifted & 0xFFFFFFFFull : shifted & ((1ull << (size * 8)) - 1);
}

void MemoryController::deviceWrite(uint64_t address, uint64_t value, unsigned size)
{
    if (!console_)
    {
        return;
    }
    const uint32_t offset = static_cast<uint32_t>(address - kConsoleBase);
    if (size == 8)
    {
        console_->write(offset, static_cast<uint32_t>(value));
        console_->write(offset + 4, static_cast<uint32_t>(value >> 32));
        return;
    }
    console_->write(offset & ~3u, static_cast<uint32_t>(value));
}

void MemoryController::reset()
{
    ioEvents_.clear();
    memory_.reset();
    l1_cache_.reset();
    l2_cache_.reset();
    instruction_cache_.reset();
    emit memoryResetSignal(); // this will notify views to reset themselves
}

void MemoryController::writeByte(uint64_t address, uint8_t value)
{
    if (isDevice(address))
    {
        deviceWrite(address, value, 1);
        return;
    }
    l1_cache_.writeByte(address, value);
    emit memoryUpdated(address);
}

void MemoryController::writeHalfWord(uint64_t address, uint16_t value)
{
    if (isDevice(address))
    {
        deviceWrite(address, value, 2);
        return;
    }
    l1_cache_.writeHalfWord(address, value);
    emit memoryUpdated(address);
}

void MemoryController::writeWord(uint64_t address, uint32_t value)
{
    if (isDevice(address))
    {
        deviceWrite(address, value, 4);
        return;
    }
    l1_cache_.writeWord(address, value);
    emit memoryUpdated(address);
}

void MemoryController::writeDoubleWord(uint64_t address, uint64_t value)
{
    if (isDevice(address))
    {
        deviceWrite(address, value, 8);
        return;
    }
    l1_cache_.writeDoubleWord(address, value);
    emit memoryUpdated(address);
}

uint8_t MemoryController::readByte(uint64_t address)
{
    if (isDevice(address))
    {
        return static_cast<uint8_t>(deviceRead(address, 1));
    }
    return l1_cache_.readByte(address);
}

uint16_t MemoryController::readHalfWord(uint64_t address)
{
    if (isDevice(address))
    {
        return static_cast<uint16_t>(deviceRead(address, 2));
    }
    return l1_cache_.readHalfWord(address);
}

uint32_t MemoryController::readWord(uint64_t address)
{
    if (isDevice(address))
    {
        return static_cast<uint32_t>(deviceRead(address, 4));
    }
    return l1_cache_.readWord(address);
}

uint64_t MemoryController::readDoubleWord(uint64_t address)
{
    if (isDevice(address))
    {
        return static_cast<uint64_t>(deviceRead(address, 8));
    }
    return l1_cache_.readDoubleWord(address);
}

// Functions to read memory directly with cache bypass

uint8_t MemoryController::readByte_d(uint64_t address)
{
    return memory_.readByte(address);
}

uint16_t MemoryController::readHalfWord_d(uint64_t address)
{
    return memory_.readHalfWord(address);
}

uint32_t MemoryController::readWord_d(uint64_t address)
{
    return memory_.readWord(address);
}

uint64_t MemoryController::readDoubleWord_d(uint64_t address)
{
    return memory_.readDoubleWord(address);
}

uint8_t MemoryController::peekByte(uint64_t address)
{
    if (isDevice(address))
    {
        return 0; // reading a device can have effects, and this must not
    }
    return l1_cache_.peekByte(address);
}

// function to read from instruction cache
uint32_t MemoryController::readInstruction(uint64_t address)
{
    return instruction_cache_.readWord(address);
}

// Functions to write directly to memory with cache bypass
void MemoryController::writeByte_d(uint64_t address, uint8_t value)
{
    memory_.writeByte(address, value);
    emit memoryUpdated(address);
}
void MemoryController::writeHalfWord_d(uint64_t address, uint16_t value)
{
    memory_.writeHalfWord(address, value);
    emit memoryUpdated(address);
}
void MemoryController::writeWord_d(uint64_t address, uint32_t value)
{
    memory_.writeWord(address, value);
    emit memoryUpdated(address);
}
void MemoryController::writeDoubleWord_d(uint64_t address, uint64_t value)
{
    memory_.writeDoubleWord(address, value);
    emit memoryUpdated(address);
}

void MemoryController::printMemory(const uint64_t address, unsigned int rows)
{
    memory_.printMemory(address, rows);
}

void MemoryController::dumpMemory(std::vector<std::string> args)
{
    memory_.dumpMemory(args);
}

void MemoryController::getMemoryPoint(std::string address)
{
    return memory_.getMemoryPoint(address);
}

Cache *MemoryController::getL1Cache()
{
    return &l1_cache_;
}
Cache *MemoryController::getL2Cache()
{
    return &l2_cache_;
}
Cache *MemoryController::getInstructionCache()
{
    return &instruction_cache_;
}
}