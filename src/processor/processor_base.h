/**
 * @file vm_base.h
 * @brief File containing the base class for the virtual machine
 * @author Vishank Singh, https://VishankSingh
 */
#ifndef VM_BASE_H
#define VM_BASE_H

#include "alu.h"
#include "common/assembled_program.h"
#include "memory_controller.h"
#include "processor/io_channel.h"
#include "processor/memory_timing.h"
#include "processor/processor_state.h"
#include "processor/registers.h"
#include "ui/processor_tab/circuit_scene.h"
#include "processor/processor_constants.h"
#include "common/undo_buffer.h"
#include <QList>
#include <QMap>
#include <QMutex>
#include <QString>
#include <QWaitCondition>
#include <QWidget>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <stack>
#include <string>
#include <vector>

namespace Kites
{
struct SyscallEffect;

struct RegisterChange
{
    unsigned int reg_index;
    unsigned int reg_type; // 0 for GPR, 1 for CSR, 2 for FPR
    uint64_t old_value;
    uint64_t new_value;
};

struct MemoryChange
{
    uint64_t address;
    std::vector<uint8_t> old_bytes_vec;
    std::vector<uint8_t> new_bytes_vec;
};

struct StepDelta
{
    uint64_t old_pc{};
    uint64_t new_pc{};
    unsigned int old_cycle{};
    unsigned int new_cycle{};
    unsigned int old_instructions_retired{};
    unsigned int new_instructions_retired{};
    unsigned int old_stall_cycles{};
    unsigned int new_stall_cycles{};
    unsigned int old_branch_mispredictions{};
    unsigned int new_branch_mispredictions{};
    std::vector<RegisterChange> register_changes{};
    std::vector<MemoryChange> memory_changes{};
    /// What the step printed or read, so that undo and redo can respect it (see io_channel.h).
    std::vector<IoEvent> io_events{};
    /// Set if the step was an exit syscall.
    bool exited{false};
    int exit_code{0};
};

/**
 * @brief Base class for the virtual machine.
 */
class ProcessorBase : public QObject
{
    Q_OBJECT
  public:
    ProcessorBase();
    ~ProcessorBase() = default;

    AssembledProgram program_;
    std::atomic<bool> stop_requested_ {false};
    std::atomic<bool> pause_requested_ {false};

    QMutex pause_mutex_;
    QWaitCondition pause_wait_condition_;

    /// Where ecalls and the memory-mapped console print and read. Owned jointly with whoever shows
    /// it (the GUI console, the CLI), so it outlives the processor when the model is switched.
    std::shared_ptr<IoChannel> io_ = std::make_shared<IoChannel>();

    /// Set once the program has made an exit syscall; the machine then counts as finished.
    bool exited_{false};
    int exit_code_{0};

    std::vector<uint64_t> breakpoints_;
    std::optional<uint64_t> last_breakpoint_pc_{std::nullopt}; // Track last PC where we paused at breakpoint
    // This is to prevent multiple pause signals when we are paused at a breakpoint and the user
    // tries to resume but the PC hasn't moved yet

    uint32_t current_instruction_{};
    uint64_t program_counter_{};
    uint64_t last_executed_pc_{INVALID_PC}; 
    ProcessorState processor_state_{};

    unsigned int step_delay_{1000}; // well change it later to get it from config
    unsigned int cycle_s_{};
    unsigned int instructions_retired_{};
    double cpi_{};
    double ipc_{};
    unsigned int stall_cycles_{};
    unsigned int branch_mispredictions_{};

    std::string output_status_;

    MemoryController memory_controller_;
    RegisterFile registers_;

    /// How long loads take. Off by default, so a cache miss costs nothing (see memory_timing.h).
    MemoryTiming memory_timing_;

    alu::Alu alu_;

    QList<QString> active_wires_{};
    size_t always_active_wires_count_{};
    // the list of wire that will be active in this cycle of vm
    // well send this to the gui to highlight those wires


    std::unique_ptr<CircuitScene> circuit_scene_; // Circuit scene for visualization
    UndoBuffer<StepDelta> m_undoBuffer{100};
    void LoadProgram(const AssembledProgram &program);
    uint64_t program_size_ = 0;

    uint64_t GetProgramCounter() const;
    void UpdateProgramCounter(int64_t value);

    int32_t ImmGenerator(uint32_t instruction);

    /**@brief Sets the breakpoints vector
     */
    void SetBreakpoints(const std::vector<uint64_t> &breakpoints);
    void AddBreakpoint(uint64_t val, bool is_line = true);
    void RemoveBreakpoint(uint64_t val, bool is_line = true);
    bool CheckBreakpoint(uint64_t address);

    void RequestStop();
    void RequestPause();
    void RequestResume();

    bool IsStopRequested() const;
    void ClearStop();
    virtual void SetActiveWireNames() = 0;
    virtual void setProcessorState() = 0;

    /// Replaces the I/O channel (the processor and its memory both use the new one).
    void SetIoChannel(std::shared_ptr<IoChannel> io);
    IoChannel &Io()
    {
        return *io_;
    }

    /// Puts what a syscall did into the step being recorded.
    void RecordSyscall(StepDelta &delta, const SyscallEffect &effect);
    /// Undo / redo of the I/O of a step: takes back output and gives back input, or the reverse.
    void UndoIo(const StepDelta &delta);
    void RedoIo(const StepDelta &delta);
    /// Moves what reads and writes of the memory-mapped console did this step into the record.
    void CollectDeviceIo(StepDelta &delta);
    /// True if the instruction at `pc` is an ecall that would have to wait for input that has not
    /// been typed, and this machine may not wait (the GUI's Step). Tells the console to ask.
    bool BlockedOnInput(uint32_t instruction);
    /// Clears the console output and the exit state (Reset).
    void ResetIo();

    virtual void Run()      = 0;
    virtual void DebugRun() = 0;
    virtual void Step()     = 0;
    virtual void Undo()     = 0;
    virtual void Redo()     = 0;
    virtual void Reset()    = 0;
    void DumpState(const std::filesystem::path &filename);

    void ModifyRegister(const std::string &reg_name, uint64_t value);
    /// Types a line of input for the program (what the console's Enter does).
    void PushInput(const std::string &input)
    {
        io_->pushLine(input);
    }
  signals:
    // vm state will have all the info like pc,cycles, control signals
    void processorClockedSignal(const ProcessorState &processorState);
    // this will send the list of wire that have to
    void updateCircuitStateSignal(const QList<QString> &wireList);
    // we emit this signal when vm is paused at breakpoint
    //  so that gui can chane paused button to resume
    void processorPausedAtBreakpointSignal();
};

}  // namespace Kites

#endif // VM_BASE_H
