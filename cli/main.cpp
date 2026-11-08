/**
 * @file main.cpp
 * @brief Entry point for the Kites command-line interface.
 */
#include "assembler/assembler.h"
#include "command_handler/command_handler.h"
#include "common/assembled_program.h"
#include "common/globals.h"
#include "processor/processor_manager.h"
#include "processor/processor_types.h"
#include "repl.h"
#include "suggest.h"
#include "utils/utils.h"

#include <QCoreApplication>
#include <QObject>
#include <QString>

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
void printUsage(const char *programName)
{
    std::cerr
        << "Usage: " << programName
        << " [assembly-file] [--vm <name>] [--mem-dump <hex_addr> <row_count>]... [--repl|-i]\n"
        << "  assembly-file   optional; if given without --repl, assembles, runs to\n"
        << "                  completion, and dumps state (batch mode)\n"
        << "  --vm <name>     rvss (default) | rv5-nh-nf | rv5-h-nf | rv5-nh-f | rv5-h-f\n"
        << "  --mem-dump      dump <row_count> 8-byte rows starting at <hex_addr>; "
           "repeatable (batch mode only)\n"
        << "  --repl, -i      drop into the interactive REPL instead of auto-running;\n"
        << "                  if assembly-file is given it is loaded before the prompt\n"
        << "  (running with no assembly-file also enters the REPL)\n"
        << "\n"
        << "Reordering suggestions (see docs/CLI.md):\n"
        << "  " << programName << " <assembly-file> --suggest [--vm <rv5-...>] [options]\n"
        << "  --suggest           suggest a better instruction order for the pipeline given by\n"
        << "                      --vm (default rv5-h-f), show the diff and measure both\n"
        << "  --unroll <n>        also unroll counted loops by up to n\n"
        << "  --only-live-registers  let registers nothing reads after a loop end\n"
        << "                      differently, so unrolled loops can drop repeated\n"
        << "                      address arithmetic\n"
        << "  --cold-cache        measure with cache latencies instead of ideal memory\n"
        << "  --plan-latency <n>  extra cycles per load to plan for (default 0, or 4 with\n"
        << "                      --cold-cache)\n"
        << "  --out <file>        write the suggested program to <file>\n"
        << "  --no-verify         do not run the programs on the simulator\n";
}

// Parses a non-negative integer option value, or returns -1.
int parseCount(const std::string &text)
{
    try
    {
        size_t used = 0;
        const int value = std::stoi(text, &used);
        return used == text.size() && value >= 0 ? value : -1;
    }
    catch (const std::exception &)
    {
        return -1;
    }
}

const std::unordered_map<std::string, Kites::ProcessorType> processorTypes = {
    {"rvss", Kites::ProcessorType::RVSS},
    {"rv5-nh-nf", Kites::ProcessorType::RV5Stage_NH_NF},
    {"rv5-h-nf", Kites::ProcessorType::RV5Stage_H_NF},
    {"rv5-nh-f", Kites::ProcessorType::RV5Stage_NH_F},
    {"rv5-h-f", Kites::ProcessorType::RV5Stage_H_F},
};
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    Kites::setupVmStateDirectory();

    std::string assemblyFile;
    std::string processorTypeStr = "rvss";
    std::vector<std::string> memDumpArgs;
    bool replRequested = false;
    bool vmGiven = false;
    bool suggestRequested = false;
    Kites::SuggestOptions suggest;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--vm")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Error: --vm requires a value.\n";
                printUsage(argv[0]);
                return 1;
            }
            processorTypeStr = argv[++i];
            vmGiven = true;
        }
        else if (arg == "--suggest")
        {
            suggestRequested = true;
        }
        else if (arg == "--unroll" || arg == "--plan-latency")
        {
            const int value = i + 1 < argc ? parseCount(argv[i + 1]) : -1;
            if (value < 0 || (arg == "--unroll" && value < 1))
            {
                std::cerr << "Error: " << arg << " requires a "
                          << (arg == "--unroll" ? "positive" : "non-negative") << " number.\n";
                printUsage(argv[0]);
                return 1;
            }
            ++i;
            (arg == "--unroll" ? suggest.unrollFactor : suggest.plannedLatency) = value;
        }
        else if (arg == "--cold-cache")
        {
            suggest.coldCache = true;
        }
        else if (arg == "--no-verify")
        {
            suggest.verify = false;
        }
        else if (arg == "--only-live-registers")
        {
            suggest.allowDeadRegisterChanges = true;
        }
        else if (arg == "--out")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Error: --out requires a file name.\n";
                printUsage(argv[0]);
                return 1;
            }
            suggest.outputFile = argv[++i];
        }
        else if (arg == "--mem-dump")
        {
            if (i + 2 >= argc)
            {
                std::cerr << "Error: --mem-dump requires <hex_addr> <row_count>.\n";
                printUsage(argv[0]);
                return 1;
            }
            memDumpArgs.push_back(argv[++i]);
            memDumpArgs.push_back(argv[++i]);
        }
        else if (arg == "--repl" || arg == "-i")
        {
            replRequested = true;
        }
        else if (!arg.empty() && arg[0] == '-')
        {
            std::cerr << "Error: unknown argument '" << arg << "'.\n";
            printUsage(argv[0]);
            return 1;
        }
        else if (assemblyFile.empty())
        {
            assemblyFile = arg;
        }
        else
        {
            std::cerr << "Error: unexpected extra argument '" << arg << "'.\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (suggestRequested && !vmGiven)
    {
        processorTypeStr = "rv5-h-f"; // suggestions are for the pipelines
    }
    auto processorTypeIt = processorTypes.find(processorTypeStr);
    if (processorTypeIt == processorTypes.end())
    {
        std::cerr << "Error: unknown VM type '" << processorTypeStr << "'.\n";
        printUsage(argv[0]);
        return 1;
    }

    if (suggestRequested)
    {
        if (assemblyFile.empty())
        {
            std::cerr << "Error: --suggest needs an assembly file.\n";
            printUsage(argv[0]);
            return 1;
        }
        suggest.assemblyFile = assemblyFile;
        suggest.processor = processorTypeIt->second;
        suggest.processorName = processorTypeStr;
        return Kites::RunSuggest(suggest);
    }

    if (replRequested || assemblyFile.empty())
    {
        Kites::ProcessorManager manager(nullptr, processorTypeIt->second);
        manager.setStepDelay(0);

        Kites::command_handler::ReplState state;
        if (!assemblyFile.empty())
        {
            try
            {
                Kites::AssembledProgram program = Kites::assemble(assemblyFile);
                manager.loadProgram(program);
                state.currentProgram = program;
                state.programLoaded = true;
                std::cout << "Loaded '" << assemblyFile << "': " << program.text_buffer.size()
                           << " instruction word(s), " << program.data_buffer.size()
                           << " data item(s).\n";
            }
            catch (const std::exception &e)
            {
                std::cerr << "Assembly failed: " << e.what() << "\n";
            }
        }

        return Kites::RunRepl(manager, state);
    }

    Kites::AssembledProgram program;
    try
    {
        program = Kites::assemble(assemblyFile);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Assembly failed: " << e.what() << std::endl;
        return 1;
    }

    Kites::ProcessorManager manager(nullptr, processorTypeIt->second);

    bool runFailed = false;
    std::string runErrorMsg;
    QObject::connect(&manager, &Kites::ProcessorManager::runErrorSignal, &manager,
                      [&](const QString &message, int /*sourceLine*/)
                      {
                          runFailed = true;
                          runErrorMsg = message.toStdString();
                      });

    // ProcessorBase::step_delay_ defaults to 1000ms and is meant to pace the
    // GUI's per-instruction animation; it must be zeroed here or an N-instruction
    // program takes N seconds to run.
    manager.setStepDelay(0);
    manager.loadProgram(program);

    // Known limitation: a program hitting a blocking read syscall will hang here
    // forever, since nothing in this CLI feeds terminal stdin into the VM's input
    // queue yet (that's separate, future work).
    manager.run();

    if (runFailed)
    {
        std::cerr << "Run failed: " << runErrorMsg << std::endl;
    }

    Kites::DumpRegisters(Kites::globals::registers_dump_file_path, *manager.getRegisterFile());

    if (!memDumpArgs.empty())
    {
        try
        {
            manager.getMemoryController()->dumpMemory(memDumpArgs);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Memory dump failed: " << e.what() << std::endl;
            return 3;
        }
    }

    std::cout << "Assembled '" << assemblyFile << "': " << program.text_buffer.size()
               << " instruction word(s), " << program.data_buffer.size() << " data item(s).\n"
               << "Run " << (runFailed ? "failed" : "completed") << ".\n"
               << "Registers   -> " << Kites::globals::registers_dump_file_path.string() << "\n"
               << "Disassembly -> " << Kites::globals::disassembly_file_path.string() << "\n"
               << "Errors      -> " << Kites::globals::errors_dump_file_path.string();
    if (!memDumpArgs.empty())
    {
        std::cout << "\nMemory      -> " << Kites::globals::memory_dump_file_path.string();
    }
    std::cout << std::endl;

    return runFailed ? 2 : 0;
}
