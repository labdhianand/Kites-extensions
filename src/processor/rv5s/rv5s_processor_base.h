#pragma once
#include "processor/pipeline_registers.h"
#include "processor/processor_base.h"
#include "processor/processor_manager.h"
#include "rv5s_control_unit.h"

#include <array>

namespace Kites
{
/// The memory-latency state of the pipeline (see memory_timing.h), saved with every step so that
/// undo and redo restore it exactly.
struct MemoryLatencyState
{
    /// Cycles the whole pipeline still has to wait for a blocking load.
    unsigned freezeCycles{0};

    /// For non-blocking loads: the first cycle in which an instruction reading each register may
    /// leave decode (32 GPRs then 32 FPRs). 0 means the register is not waiting on a load.
    std::array<uint64_t, 64> readyCycle{};

    bool operator==(const MemoryLatencyState &other) const = default;
};

struct PipelineRegisterChange
{
    IF_ID_Register old_if_id_reg;
    IF_ID_Register new_if_id_reg;

    ID_EX_Register old_id_ex_reg;
    ID_EX_Register new_id_ex_reg;

    EX_MEM_Register old_ex_mem_reg;
    EX_MEM_Register new_ex_mem_reg;

    MEM_WB_Register old_mem_wb_reg;
    MEM_WB_Register new_mem_wb_reg;
};

struct RV5StageStepDelta : public StepDelta
{
    PipelineRegisterChange pipeline_register_change;
    MemoryLatencyState old_memory_latency;
    MemoryLatencyState new_memory_latency;
};

class RV5StageVM_Base : public ProcessorBase
{
  public:
    RV5StageVM_Base() = default;
    ~RV5StageVM_Base() = default;

    // --- VM Control Functions ---

    void PrintType()
    {
        std::cout << "rv5s_vm" << std::endl;
    }
    void DebugRun() override;
    void Reset() override;
    void Undo() override;
    void Redo() override;

  protected:
    // Pipeline Registers
    IF_ID_Register if_id_reg_;
    ID_EX_Register id_ex_reg_;
    EX_MEM_Register ex_mem_reg_;
    MEM_WB_Register mem_wb_reg_;

    // The control unit for the pipeline
    RV5SControlUnit control_unit_;

    std::stack<RV5StageStepDelta> undo_stack_{};
    std::stack<RV5StageStepDelta> redo_stack_{};
    RV5StageStepDelta current_delta_;

    // Set when EX/MEM redirected the PC this cycle; consumed by fetch_and_advance_pc().
    bool pc_redirected_ = false;

    MemoryLatencyState memory_latency_;

    // What this pipeline variant has, which decides how a slow load is waited for.
    virtual bool has_hazard_detection() const
    {
        return false;
    }
    virtual bool has_forwarding() const
    {
        return false;
    }

    // If a blocking load is still being waited for, spends this whole cycle waiting and returns
    // true. Every Step() calls this first.
    bool wait_for_blocking_load();

    // True if `instruction` reads a register whose non-blocking load has not arrived yet, so it
    // has to stay in decode this cycle. Only variants with hazard detection call this.
    bool waits_for_late_load(uint32_t instruction) const;

    // Charges the extra latency of the load that just accessed memory in MEM, given the miss
    // counters of L1 and L2 from before the access.
    void charge_load_latency(size_t l1MissesBefore, size_t l2MissesBefore);

    void memory_writeback();
    void memory_writeback_float();
    void memory_writeback_double();

    void register_write_back(const uint64_t &write_data);
    void memory_read();
    // void memory_read_float();
    // void memory_read_double();

    void begin_step_delta();
    void finalize_step_delta();

    void print_pipeline_registers_debug();
    bool is_pipeline_drained() const;
    void setProcessorState() override;
    void Run() override; // run debug run adn reset are same across all rv5s vms

    // --- Private methods for each pipeline stage ---
    virtual void pipeline_fetch() = 0;

    void redirect_pc(uint64_t target);
    void fetch_and_advance_pc(bool stalled);

    void pipeline_decode();

    virtual void pipeline_execute() = 0;
    virtual uint64_t pipeline_execute_float() = 0;
    virtual uint64_t pipeline_execute_double() = 0;

    uint64_t execute_float();
    uint64_t execute_double();

    void execute_csr() {};

    void pipeline_memory();
    void pipeline_writeback();
    void pipeline_writeback_float();
    void pipeline_writeback_double();

    // --- Specialized handler functions (called from pipeline stages) ---
    virtual void handle_syscall() = 0;
};
}//namespace Kites