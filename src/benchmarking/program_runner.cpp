/**
 * @file program_runner.cpp
 * @brief Implementation of the program runner.
 */
#include "benchmarking/program_runner.h"

#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rv5s/rv5s_processor_nh_f.h"
#include "processor/rv5s/rv5s_processor_nh_nf.h"
#include "processor/rvss/rvss_processor.h"

namespace Kites
{
namespace benchmarking
{
namespace
{
constexpr uint64_t kDataStart = 0x10000000;
constexpr uint64_t kDataBytesCaptured = 1024;

// The simulator prints a line per cycle while it runs (to stdout and stderr); that is not wanted
// in a report.
class QuietOutput
{
  public:
    QuietOutput()
        : m_previous(std::cout.rdbuf(m_sink.rdbuf())), m_previousErr(std::cerr.rdbuf(m_sink.rdbuf()))
    {
    }
    ~QuietOutput()
    {
        std::cout.rdbuf(m_previous);
        std::cerr.rdbuf(m_previousErr);
    }

  private:
    std::ostringstream m_sink;
    std::streambuf *m_previous;
    std::streambuf *m_previousErr;
};

std::unique_ptr<ProcessorBase> createProcessor(ProcessorType type)
{
    switch (type)
    {
    case ProcessorType::RV5Stage_NH_NF:
        return std::make_unique<RV5StageProcessorNHNF>();
    case ProcessorType::RV5Stage_H_NF:
        return std::make_unique<RV5StageProcessorHNF>();
    case ProcessorType::RV5Stage_NH_F:
        return std::make_unique<RV5StageProcessorNHF>();
    case ProcessorType::RV5Stage_H_F:
        return std::make_unique<RV5StageProcessorHF>();
    default:
        return std::make_unique<RVSSProcessor>();
    }
}

void configureMemory(ProcessorBase &vm, MemorySetup memory)
{
    if (memory != MemorySetup::ColdCache)
    {
        return;
    }
    vm.memory_controller_.getL2Cache()->reconfigure(CacheConfig{
        32, 16, 4, WritePolicy::WriteBack, AllocationPolicy::WriteAllocate, ReplacementPolicy::LRU});
    vm.memory_controller_.getL1Cache()->reconfigure(CacheConfig{
        4, 16, 2, WritePolicy::WriteBack, AllocationPolicy::WriteAllocate, ReplacementPolicy::LRU});
    vm.memory_timing_.enabled = true;
    vm.memory_timing_.nonBlocking = true;
    vm.memory_timing_.l1HitCycles = 0;
    vm.memory_timing_.l2HitCycles = kColdCacheL2Latency;
    vm.memory_timing_.memoryCycles = 20;
}
} // namespace

std::string describe(MemorySetup setup)
{
    if (setup == MemorySetup::ColdCache)
    {
        return "cold cache (L1 128 B +0, L2 2 KB +4, memory +20 cycles per load)";
    }
    return "ideal memory (every access takes one cycle)";
}

std::string describe(RunEnd end)
{
    switch (end)
    {
    case RunEnd::NeedsInput:
        return "the program asks for input that was not given";
    case RunEnd::CycleLimit:
        return "the program did not end within " + std::to_string(kCycleLimit) +
               " cycles (does it loop for ever, or wait for a key that is never pressed?)";
    default:
        return "the program finished";
    }
}

ProgramRun runProgram(const std::string &source, ProcessorType type, MemorySetup memory,
                      const std::string &input, uint64_t cycleLimit)
{
    std::istringstream stream(source);
    const AssembledProgram program = assemble(stream);

    QuietOutput quiet;
    std::unique_ptr<ProcessorBase> vm = createProcessor(type);
    configureMemory(*vm, memory);
    vm->LoadProgram(program);
    vm->cycle_limit_ = cycleLimit;
    vm->Io().setBlocking(false); // never wait for a user who is not there
    // The last word needs the Enter that ends it, or a read would wait for more digits.
    vm->Io().pushRaw(!input.empty() && input.back() != '\n' ? input + "\n" : input);
    vm->DebugRun();

    ProgramRun run;
    run.output = vm->Io().output();
    run.end = vm->waiting_for_input_ ? RunEnd::NeedsInput
              : vm->CycleLimitReached() ? RunEnd::CycleLimit
                                        : RunEnd::Finished;
    for (size_t i = 0; i < 32; ++i)
    {
        run.state.gprs[i] = vm->registers_.ReadGpr(i);
    }
    for (uint64_t offset = 0; offset < kDataBytesCaptured; offset += 4)
    {
        run.state.data.push_back(vm->memory_controller_.readWord(kDataStart + offset));
    }
    run.cycles = vm->cycle_s_;
    return run;
}

bool sameResult(const ProgramState &expected, const ProgramState &actual,
                const std::vector<unsigned> &ignoredRegisters)
{
    ProgramState left = expected;
    ProgramState right = actual;
    for (unsigned reg : ignoredRegisters)
    {
        if (reg < 32)
        {
            left.gprs[reg] = right.gprs[reg] = 0;
        }
    }
    return left == right;
}

} // namespace benchmarking
} // namespace Kites
