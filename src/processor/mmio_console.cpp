#include "processor/mmio_console.h"

namespace Kites
{
uint32_t ConsoleDevice::read(uint32_t offset)
{
    if (!io_)
    {
        return 0;
    }
    switch (offset)
    {
    case KbdStatus:
        // With the terminal as input a program polling for a key would wait for ever, because
        // nothing else reads the terminal, so asking waits for a line to be typed.
        return io_->waitForChar() ? 1u : 0u;
    case KbdData:
    {
        char c = 0;
        if (!io_->takeChar(c))
        {
            return 0;
        }
        events_->push_back(IoEvent{IoEvent::Kind::Input, std::string(1, c)});
        return static_cast<uint8_t>(c);
    }
    case DispStatus:
        return 1;
    default:
        return 0;
    }
}

void ConsoleDevice::write(uint32_t offset, uint32_t value)
{
    if (!io_ || offset != DispData)
    {
        return;
    }
    const std::string text(1, static_cast<char>(value & 0xFF));
    io_->write(text);
    events_->push_back(IoEvent{IoEvent::Kind::Output, text});
}

} // namespace Kites
