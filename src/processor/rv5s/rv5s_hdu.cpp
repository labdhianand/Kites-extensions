/**
 * @file hazard_detection_unit.cpp
 * @brief Logic for the Hazard Detection Unit (HDU).
 * * This function determines if a data hazard exists between instructions in
 * the ID/EX and IF/ID pipeline registers, and calculates the number of stalls
 * (bubbles) needed based on the simulator's mode (Forwarding ON/OFF).
 * * NOTE: This version includes and uses the full structs from pipeline_registers.h.
 */
#include <algorithm>
#include <cstdint>
#include <iostream>


#include "common/instructions.h"
#include "optimizer/instr_info.h"
#include "processor/pipeline_registers.h"

namespace Kites
{
// Define stall count constants
constexpr int STALL_NONE = 0;
constexpr int STALL_ONE_CYCLE = 1;
constexpr int STALL_TWO_CYCLES = 2;

/**
 * @brief Checks for data hazards and returns the number of stalls required.
 * @param if_id_reg The register holding the instruction in the IF/ID stage (the dependent
 * instruction).
 * @param id_ex_reg The register holding the instruction in the ID/EX stage (the source
 * instruction).
 * @param is_forwarding_enabled Flag to switch between 'Forwarding ON' and 'No Forwarding' modes.
 * @return The number of cycles the pipeline should stall (0, 1, or 2).
 */
int check_data_hazard(const IF_ID_Register &if_id_reg, const ID_EX_Register &id_ex_reg,
                      const EX_MEM_Register &ex_mem_reg, bool is_forwarding_enabled)
{
    // --- 1. Extract Source Register Indices from IF/ID Instruction (Dependent) ---
    uint32_t instruction = if_id_reg.instruction;

    // GPR Source Registers (rs1/rs2 fields)
    uint8_t if_id_rs1 = (instruction >> 15) & 0x1F;
    uint8_t if_id_rs2 = (instruction >> 20) & 0x1F;

    // Integer instructions read only the registers they really encode. In the I, S, U and J
    // formats the bits at the rs1/rs2 positions are part of an immediate, so comparing them with
    // register numbers reports hazards that do not exist (for example `lw x8, 8(x10)` right after
    // a write to x8). Floating-point instructions keep the original field-based check.
    const bool is_float_consumer = instruction_set::isFInstruction(instruction) ||
                                   instruction_set::isDInstruction(instruction);
    const optimizer::InstrInfo consumer_info = optimizer::decodeInstruction(instruction, 0);
    auto consumer_reads_gpr = [&](uint8_t reg)
    {
        if (is_float_consumer)
        {
            return reg == if_id_rs1 || reg == if_id_rs2;
        }
        for (size_t i = 0; i < consumer_info.sourceCount; ++i)
        {
            if (consumer_info.sources[i].file == optimizer::RegFile::Gpr &&
                consumer_info.sources[i].index == reg)
            {
                return true;
            }
        }
        return false;
    };

    // FPR Source Registers (frs1/frs2/frs3 fields)
    // Note: F-type instructions reuse the rs1/rs2 bit-fields for frs1/frs2
    uint8_t if_id_frs1 = (instruction >> 15) & 0x1F;
    uint8_t if_id_frs2 = (instruction >> 20) & 0x1F;
    uint8_t if_id_frs3 = (instruction >> 27) & 0x1F; // For FMA (R4 format)

    // --- 2. Check for GPR Hazard (Source writes GPR, Dependent reads GPR) ---
    // Two-cycle hazard: current EX/MEM instruction writes a GPR the IF/ID instruction reads
    bool gpr_hazard_two_cycle =
        (ex_mem_reg.reg_write && (ex_mem_reg.rd != 0)) && consumer_reads_gpr(ex_mem_reg.rd);

    // One-cycle hazard: previous EX/MEM instruction (now further down) writes a GPR the IF/ID
    // instruction reads
    bool gpr_hazard_one_cycle = (ex_mem_reg.prev_reg_write && (ex_mem_reg.prev_rd != 0)) &&
                                consumer_reads_gpr(ex_mem_reg.prev_rd);

    bool gpr_hazard = gpr_hazard_one_cycle || gpr_hazard_two_cycle;

    // --- 3. Check for FPR Hazard (Source writes FPR, Dependent reads FPR) ---
    // Note: FPR f0 is NOT hardwired to zero (unlike GPR x0), so no frd != 0 check needed.
    // Two-cycle hazard: current EX/MEM instruction writes an FPR the IF/ID instruction reads
    bool fpr_hazard_two_cycle =
        is_float_consumer && (ex_mem_reg.freg_write) &&
        (ex_mem_reg.frd == if_id_frs1 || ex_mem_reg.frd == if_id_frs2 ||
         ex_mem_reg.frd == if_id_frs3);

    // One-cycle hazard: previous EX/MEM instruction writes an FPR the IF/ID instruction reads
    bool fpr_hazard_one_cycle =
        is_float_consumer && (ex_mem_reg.prev_freg_write) &&
        (ex_mem_reg.prev_frd == if_id_frs1 || ex_mem_reg.prev_frd == if_id_frs2 ||
         ex_mem_reg.prev_frd == if_id_frs3);

    bool fpr_hazard = fpr_hazard_one_cycle || fpr_hazard_two_cycle;

    // --- 4. Determine Stall Count ---
    // Combined hazard flag (either GPR or FPR dependency)
    bool any_hazard_two_cycle = gpr_hazard_two_cycle || fpr_hazard_two_cycle;
    bool any_hazard_one_cycle = gpr_hazard_one_cycle || fpr_hazard_one_cycle;
    bool any_hazard = gpr_hazard || fpr_hazard;

    if (is_forwarding_enabled)
    {
        // --- Forwarding ON: Detects Load-Use only (1 Stall) ---
        // Load-Use hazard occurs if the producing instruction (ID/EX) is a Load
        // AND the dependent instruction reads that destination register.
        if (id_ex_reg.mem_read)
        {
            // Check if the load destination matches any source of the dependent instruction
            bool gpr_load_use = (id_ex_reg.reg_write && id_ex_reg.rd != 0) &&
                                consumer_reads_gpr(id_ex_reg.rd);

            bool fpr_load_use = is_float_consumer && (id_ex_reg.freg_write) &&
                                (id_ex_reg.frd == if_id_frs1 || id_ex_reg.frd == if_id_frs2 ||
                                 id_ex_reg.frd == if_id_frs3);

            if (gpr_load_use || fpr_load_use)
            {
                return STALL_ONE_CYCLE;
            }
        }

        // All other hazards (R-R, F-F, R-Store, F-Store) are solved by the forwarding unit.
        return STALL_NONE;
    }
    else if (!any_hazard)
    {
        return STALL_NONE;
    }
    else if (any_hazard_two_cycle)
    {
        return STALL_TWO_CYCLES;
    }
    else if (any_hazard_one_cycle)
    {
        return STALL_ONE_CYCLE;
    }

    return STALL_NONE;
}
}//namespace Kites