/**
 * @file syscall_handler.h
 * @brief What `ecall` does, shared by all five processor models.
 *
 * The cores differ in *when* an ecall runs (the single-cycle core executes it in its one step,
 * the pipelines when it retires) but not in *what* it does, so that lives here once. The handler
 * does not change the machine itself: it returns an effect, and each core applies the effect to
 * its own registers, memory and undo record.
 *
 * Calling convention (a7 = number, arguments in a0-a2 and fa0, result in a0 or fa0):
 *
 *   1  print_int     a0 = value                      (signed)
 *   2  print_float   fa0 = value
 *   3  print_double  fa0 = value
 *   4  print_string  a0 = address of a NUL-terminated string
 *   5  read_int      -> a0
 *   6  read_float    -> fa0
 *   7  read_double   -> fa0
 *   8  read_string   a0 = buffer, a1 = size; reads a line, NUL-terminated (at most size-1 chars)
 *  10  exit
 *  11  print_char   a0 = character
 *  12  read_char    -> a0
 *  63  read          a0 = fd (0), a1 = buffer, a2 = count; -> a0 = bytes read
 *  64  write         a0 = fd (1 or 2), a1 = buffer, a2 = count; -> a0 = bytes written
 *  93  exit          a0 = exit code
 */
#pragma once

#include "processor/io_channel.h"
#include "processor/memory_controller.h"
#include "processor/processor_base.h"
#include "processor/registers.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

namespace Kites
{
/// What one ecall did, for the core to keep for undo and redo.
struct SyscallEffect
{
    std::vector<RegisterChange> registerChanges; ///< registers the syscall wrote
    std::vector<MemoryChange> memoryChanges;     ///< memory the syscall wrote
    std::optional<IoEvent> io;                   ///< what it printed or consumed
    bool exited{false};
    int exitCode{0};
    bool cancelled{false}; ///< stopped while waiting for input: nothing happened
    bool unknown{false};   ///< a7 is not a syscall number
};

class SyscallHandler
{
  public:
    /// Whether an ecall with the current registers would have to wait for input that is not
    /// there. Lets a core refuse to start the ecall instead of freezing in the middle of it.
    static bool wouldWait(const RegisterFile &registers, const IoChannel &io);

    /// Runs the ecall: writes any result registers and memory, talks to the channel, and returns
    /// what it did so the core can record it for undo. `cancel` is polled while waiting for input.
    static SyscallEffect execute(RegisterFile &registers, MemoryController &memory, IoChannel &io,
                                 const std::atomic<bool> *cancel);
};

} // namespace Kites
