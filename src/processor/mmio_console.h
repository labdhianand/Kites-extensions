/**
 * @file mmio_console.h
 * @brief A keyboard and a display on the memory bus, for programs that poll instead of using ecall.
 *
 * Both registers are 32 bits wide. A program waits for a key by reading KBD_STATUS until bit 0 is
 * set, then reads KBD_DATA, which takes the key out of the input. It prints by writing a character
 * to DISP_DATA. Accesses go straight to the device: they never touch the caches.
 *
 *   0x20000000  KBD_STATUS   read   bit 0 = a character is waiting
 *   0x20000004  KBD_DATA     read   next character (0 if none); taking it consumes it
 *   0x20000008  DISP_STATUS  read   bit 0 = ready (always 1)
 *   0x2000000C  DISP_DATA    write  low byte is printed
 *
 * The base address is below 0x80000000 so that `lui t0, 0x20000` reaches it.
 */
#pragma once

#include "processor/io_channel.h"
#include "processor/mmio_devices.h"

#include <vector>

namespace Kites
{
constexpr uint32_t kConsoleBase = 0x20000000;
constexpr uint32_t kConsoleSize = 0x10;

class ConsoleDevice : public MMIODevice
{
  public:
    enum Register : uint32_t
    {
        KbdStatus = 0x0,
        KbdData = 0x4,
        DispStatus = 0x8,
        DispData = 0xC,
    };

    /// `events` collects what the accesses did, for the step that made them to keep.
    ConsoleDevice(IoChannel *io, std::vector<IoEvent> *events) : io_(io), events_(events)
    {
    }

    uint32_t read(uint32_t offset) override;
    void write(uint32_t offset, uint32_t value) override;

    bool isReady() const override
    {
        return true;
    }
    uint32_t size() const override
    {
        return kConsoleSize;
    }
    uint32_t baseAddress() const override
    {
        return kConsoleBase;
    }
    const char *name() const override
    {
        return "console";
    }

  private:
    IoChannel *io_;
    std::vector<IoEvent> *events_;
};

} // namespace Kites
