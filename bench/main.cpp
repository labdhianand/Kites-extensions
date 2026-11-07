/**
 * @file main.cpp
 * @brief kites-bench: measures the reordering optimizer on the benchmark programs.
 *
 * For every benchmark, pipeline variant and memory setting it runs the original program, the
 * reordered program, and the reordered and unrolled program on the simulator, checks that each
 * computes exactly what the original computes on the single-cycle processor, and writes the
 * measured cycle counts as a Markdown table and as CSV.
 *
 * On the variants without hazard detection the original is first padded with just the NOPs it
 * needs to be correct (the optimizer's padOnly mode), so the comparison is against carefully
 * padded code, not against code that computes the wrong result.
 *
 * Usage: kites-bench <benchmarks-dir> [--markdown <file>] [--csv <file>]
 */
#include "assembler/assembler.h"
#include "optimizer/optimizer.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rv5s/rv5s_processor_nh_f.h"
#include "processor/rv5s/rv5s_processor_nh_nf.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace Kites;
using namespace Kites::optimizer;

namespace
{
constexpr uint64_t kDataStart = 0x10000000;
constexpr uint64_t kDataBytesChecked = 1024;
constexpr int kUnrollFactor = 4;

// Everything a program leaves behind that the benchmarks care about.
struct Snapshot
{
    std::array<uint64_t, 32> gprs{};
    std::vector<uint32_t> data;
};

struct Run
{
    Snapshot snapshot;
    unsigned cycles{0};
};

// A memory setting to measure under.
struct MemorySetting
{
    std::string title;
    std::string description;
    bool coldCache{false};
    int plannedLatency{0}; ///< what the optimizer is told to plan for on every load
};

const std::vector<MemorySetting> kMemorySettings = {
    {"Ideal memory", "Every access fits in the one-cycle MEM stage (the simulator's default).", false,
     0},
    {"Cold cache",
     "L1: 4 sets x 2 ways x 16 B (128 B). L2: 32 sets x 4 ways x 16 B (2 KB). Extra cycles per "
     "load: L1 hit +0, L2 hit +4, main memory +20. Loads are non-blocking on the variants with "
     "hazard detection and blocking on the others. The data starts in main memory only. The "
     "optimizer plans for the L2 latency (+4).",
     true, 4},
};

struct VariantInfo
{
    const char *name;
    PipelineVariant variant;
};

const std::vector<VariantInfo> kVariants = {{"NH_NF", PipelineVariant::NH_NF},
                                            {"H_NF", PipelineVariant::H_NF},
                                            {"NH_F", PipelineVariant::NH_F},
                                            {"H_F", PipelineVariant::H_F}};

// Silences the simulator's own per-cycle output.
class QuietOutput
{
  public:
    QuietOutput() : m_previous(std::cout.rdbuf(m_sink.rdbuf()))
    {
    }
    ~QuietOutput()
    {
        std::cout.rdbuf(m_previous);
    }

  private:
    std::ostringstream m_sink;
    std::streambuf *m_previous;
};

void configureMemory(ProcessorBase &vm, const MemorySetting &setting)
{
    if (!setting.coldCache)
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
    vm.memory_timing_.l2HitCycles = 4;
    vm.memory_timing_.memoryCycles = 20;
}

template <typename Vm> Run runOn(const std::string &source, const MemorySetting &setting)
{
    std::istringstream stream(source);
    const AssembledProgram program = assemble(stream);

    QuietOutput quiet;
    Vm vm;
    configureMemory(vm, setting);
    vm.LoadProgram(program);
    vm.DebugRun();

    Run run;
    for (size_t i = 0; i < 32; ++i)
    {
        run.snapshot.gprs[i] = vm.registers_.ReadGpr(i);
    }
    for (uint64_t offset = 0; offset < kDataBytesChecked; offset += 4)
    {
        run.snapshot.data.push_back(vm.memory_controller_.readWord(kDataStart + offset));
    }
    run.cycles = vm.cycle_s_;
    return run;
}

Run runOnVariant(PipelineVariant variant, const std::string &source, const MemorySetting &setting)
{
    switch (variant)
    {
    case PipelineVariant::NH_NF:
        return runOn<RV5StageProcessorNHNF>(source, setting);
    case PipelineVariant::H_NF:
        return runOn<RV5StageProcessorHNF>(source, setting);
    case PipelineVariant::NH_F:
        return runOn<RV5StageProcessorNHF>(source, setting);
    default:
        return runOn<RV5StageProcessorHF>(source, setting);
    }
}

// Same registers and data, ignoring registers the unroller started using (the original never
// touched them, so their final values are not part of what it computes).
bool sameResult(const Snapshot &reference, const Snapshot &actual,
                const std::vector<LoopReport> &loops)
{
    Snapshot left = reference;
    Snapshot right = actual;
    for (const LoopReport &loop : loops)
    {
        for (unsigned reg : loop.renamedTo)
        {
            left.gprs[reg] = right.gprs[reg] = 0;
        }
    }
    return left.gprs == right.gprs && left.data == right.data;
}

struct Measurement
{
    std::string benchmark;
    std::string memory;
    std::string variant;
    unsigned original{0};
    unsigned reordered{0};
    unsigned unrolled{0};
    int unrolledLoops{0};
    bool correct{true};
};

std::string percent(unsigned before, unsigned after)
{
    std::ostringstream out;
    const double change = 100.0 * (static_cast<double>(after) - before) / before;
    out << std::fixed << std::setprecision(1) << (change > 0 ? "+" : "") << change << "%";
    return out.str();
}

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void writeMarkdown(const std::filesystem::path &path, const std::vector<Measurement> &results)
{
    std::ofstream out(path);
    out << "# Benchmark results\n\n"
        << "Generated by `kites-bench` from the programs in `benchmarks/`. Do not edit by hand; "
           "re-run the tool instead.\n\n"
        << "Cycles measured on the Kites simulator. **Original** is the program as written "
           "(on NH_NF and NH_F padded with just the NOPs it needs to be correct). **Reordered** "
           "is the optimizer's suggestion. **+ unrolled** also unrolls counted loops by up to "
           "x" << kUnrollFactor
        << ". Every version was checked to compute exactly what the original computes.\n";

    for (const MemorySetting &setting : kMemorySettings)
    {
        out << "\n## " << setting.title << "\n\n" << setting.description << "\n\n";
        out << "| Benchmark | Variant | Original | Reordered | Change | + unrolled | Change |\n"
            << "|---|---|---:|---:|---:|---:|---:|\n";
        for (const Measurement &m : results)
        {
            if (m.memory != setting.title)
            {
                continue;
            }
            out << "| " << m.benchmark << " | " << m.variant << " | " << m.original << " | "
                << m.reordered << " | " << percent(m.original, m.reordered) << " | " << m.unrolled
                << " | " << percent(m.original, m.unrolled) << " |\n";
        }
    }
}

void writeCsv(const std::filesystem::path &path, const std::vector<Measurement> &results)
{
    std::ofstream out(path);
    out << "benchmark,memory,variant,original_cycles,reordered_cycles,unrolled_cycles,"
           "unrolled_loops,correct\n";
    for (const Measurement &m : results)
    {
        out << m.benchmark << "," << m.memory << "," << m.variant << "," << m.original << ","
            << m.reordered << "," << m.unrolled << "," << m.unrolledLoops << ","
            << (m.correct ? "yes" : "no") << "\n";
    }
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    setupVmStateDirectory();

    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <benchmarks-dir> [--markdown <file>] [--csv <file>]\n";
        return 1;
    }
    const std::filesystem::path directory = argv[1];
    std::filesystem::path markdownPath;
    std::filesystem::path csvPath;
    for (int i = 2; i + 1 < argc; i += 2)
    {
        const std::string flag = argv[i];
        if (flag == "--markdown")
        {
            markdownPath = argv[i + 1];
        }
        else if (flag == "--csv")
        {
            csvPath = argv[i + 1];
        }
        else
        {
            std::cerr << "Unknown option " << flag << "\n";
            return 1;
        }
    }

    std::vector<std::filesystem::path> programs;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
    {
        if (entry.path().extension() == ".s")
        {
            programs.push_back(entry.path());
        }
    }
    std::sort(programs.begin(), programs.end());
    if (programs.empty())
    {
        std::cerr << "No .s files in " << directory << "\n";
        return 1;
    }

    std::vector<Measurement> results;
    bool allCorrect = true;
    for (const MemorySetting &setting : kMemorySettings)
    {
        for (const auto &path : programs)
        {
            const std::string name = path.stem().string();
            const std::string source = readFile(path);
            const Snapshot reference = runOn<RVSSProcessor>(source, MemorySetting{}).snapshot;

            for (const VariantInfo &info : kVariants)
            {
                Measurement m;
                m.benchmark = name;
                m.memory = setting.title;
                m.variant = info.name;

                OptimizeOptions options;
                options.variant = info.variant;
                options.loadExtraLatency = setting.plannedLatency;

                // The variants without hazard detection need padding to be correct at all.
                OptimizeOptions pad = options;
                pad.padOnly = true;
                const std::string original = optimizeSource(source, pad).source;

                options.dropNops = true; // padding is redone by the optimizer where it is needed
                const OptimizeResult reordered = optimizeSource(original, options);
                options.unrollFactor = kUnrollFactor;
                const OptimizeResult unrolled = optimizeSource(original, options);
                for (const LoopReport &loop : unrolled.loops)
                {
                    m.unrolledLoops += loop.unrolled ? 1 : 0;
                }

                const Run before = runOnVariant(info.variant, original, setting);
                const Run afterReorder = runOnVariant(info.variant, reordered.source, setting);
                const Run afterUnroll = runOnVariant(info.variant, unrolled.source, setting);
                m.original = before.cycles;
                m.reordered = afterReorder.cycles;
                m.unrolled = afterUnroll.cycles;
                m.correct = sameResult(reference, before.snapshot, {}) &&
                            sameResult(reference, afterReorder.snapshot, reordered.loops) &&
                            sameResult(reference, afterUnroll.snapshot, unrolled.loops);
                allCorrect = allCorrect && m.correct;

                std::cerr << std::left << std::setw(13) << setting.title << std::setw(18) << name
                          << std::setw(6) << info.name << std::right << std::setw(6) << m.original
                          << std::setw(6) << m.reordered << std::setw(6) << m.unrolled
                          << (m.correct ? "" : "   WRONG RESULT") << "\n";
                results.push_back(m);
            }
        }
    }

    if (!markdownPath.empty())
    {
        writeMarkdown(markdownPath, results);
    }
    if (!csvPath.empty())
    {
        writeCsv(csvPath, results);
    }
    if (!allCorrect)
    {
        std::cerr << "Some optimized program computed a different result.\n";
        return 2;
    }
    return 0;
}
