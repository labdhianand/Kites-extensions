/**
 * @file dependency_graph.cpp
 * @brief Implementation of the per-block dependency graph.
 */
#include "optimizer/dependency_graph.h"

#include <array>
#include <optional>

namespace Kites
{
namespace optimizer
{
namespace
{
constexpr size_t kGprCount = 32;
constexpr size_t kRegisterSlotCount = 64; // 32 GPRs followed by 32 FPRs

size_t registerSlot(RegRef reg)
{
    return (reg.file == RegFile::Fpr ? kGprCount : 0) + reg.index;
}

struct MemoryAccess
{
    size_t node{0};
    bool isStore{false};
    RegRef base{};
    uint32_t baseVersion{0}; ///< how many times the base register was written before this access
    int32_t offset{0};
    uint8_t size{0};
};

// What one unit reads from outside itself, writes, and accesses in memory.
struct NodeAccesses
{
    std::vector<RegRef> reads;
    std::vector<RegRef> writes;
    std::vector<MemoryAccess> memory;
};

void addUnique(std::vector<RegRef> &list, RegRef reg)
{
    for (const RegRef &existing : list)
    {
        if (existing == reg)
        {
            return;
        }
    }
    list.push_back(reg);
}

NodeAccesses collectAccesses(const ProgramModel &model, const Unit &unit, size_t node,
                             std::array<uint32_t, kGprCount> &baseVersions)
{
    NodeAccesses access;
    std::array<bool, kRegisterSlotCount> writtenInUnit{};

    for (size_t k = unit.firstInstruction; k < unit.firstInstruction + unit.instructionCount; ++k)
    {
        const InstrInfo &info = model.instructions[k];

        // A register an earlier instruction of the same unit wrote is not an outside dependency
        // (for example the addi of `li` reads the register its own lui just wrote).
        for (size_t s = 0; s < info.sourceCount; ++s)
        {
            if (!writtenInUnit[registerSlot(info.sources[s])])
            {
                addUnique(access.reads, info.sources[s]);
            }
        }

        if (info.isMemory())
        {
            MemoryAccess memory;
            memory.node = node;
            memory.isStore = info.klass == InstrClass::Store;
            memory.base = info.memBase;
            memory.baseVersion = baseVersions[info.memBase.index];
            memory.offset = info.imm;
            memory.size = info.memSize;
            access.memory.push_back(memory);
        }

        if (info.dest.isValid())
        {
            writtenInUnit[registerSlot(info.dest)] = true;
            addUnique(access.writes, info.dest);
            if (info.dest.file == RegFile::Gpr)
            {
                ++baseVersions[info.dest.index];
            }
        }
    }
    return access;
}

// True only if the two accesses can be shown never to touch the same byte. This holds when they
// use the same base register, nothing wrote that register in between, and their offset ranges do
// not overlap. Register dependencies keep the base writes ordered around both accesses, so the
// proof still holds after reordering.
bool provablyDisjoint(const MemoryAccess &a, const MemoryAccess &b)
{
    if (!(a.base == b.base) || a.baseVersion != b.baseVersion)
    {
        return false;
    }
    const int64_t aEnd = static_cast<int64_t>(a.offset) + a.size;
    const int64_t bEnd = static_cast<int64_t>(b.offset) + b.size;
    return aEnd <= b.offset || bEnd <= a.offset;
}

void addEdge(DependencyGraph &graph, size_t from, size_t to, DependencyKind kind)
{
    for (size_t edgeIndex : graph.outgoing[from])
    {
        const Dependency &existing = graph.edges[edgeIndex];
        if (existing.to == to && existing.kind == kind)
        {
            return;
        }
    }
    const size_t edgeIndex = graph.edges.size();
    graph.edges.push_back({from, to, kind});
    graph.outgoing[from].push_back(edgeIndex);
    graph.incoming[to].push_back(edgeIndex);
}

// Units that must keep their exact position in the block.
bool isPinned(const Unit &unit, const BasicBlock &block)
{
    return !block.isReorderable || unit.isBarrier || unit.readsPc;
}
} // namespace

bool DependencyGraph::isRespectedBy(const std::vector<size_t> &order) const
{
    if (order.size() != nodeCount)
    {
        return false;
    }

    std::vector<size_t> position(nodeCount, nodeCount);
    for (size_t slot = 0; slot < order.size(); ++slot)
    {
        if (order[slot] >= nodeCount || position[order[slot]] != nodeCount)
        {
            return false; // out of range or placed twice
        }
        position[order[slot]] = slot;
    }

    for (const Dependency &edge : edges)
    {
        if (position[edge.from] >= position[edge.to])
        {
            return false;
        }
    }
    return true;
}

DependencyGraph buildDependencyGraph(const ProgramModel &model, const BasicBlock &block)
{
    DependencyGraph graph;
    graph.nodeCount = block.unitCount;
    graph.incoming.resize(block.unitCount);
    graph.outgoing.resize(block.unitCount);

    std::array<std::optional<size_t>, kRegisterSlotCount> lastWriter{};
    std::array<std::vector<size_t>, kRegisterSlotCount> readersSinceWrite{};
    std::array<uint32_t, kGprCount> baseVersions{};
    std::vector<MemoryAccess> previousAccesses;
    std::optional<size_t> lastFence;

    for (size_t node = 0; node < block.unitCount; ++node)
    {
        const Unit &unit = model.units[block.firstUnit + node];
        const NodeAccesses access = collectAccesses(model, unit, node, baseVersions);

        for (const RegRef &reg : access.reads)
        {
            const size_t slot = registerSlot(reg);
            if (lastWriter[slot] && *lastWriter[slot] != node)
            {
                addEdge(graph, *lastWriter[slot], node, DependencyKind::RegisterRaw);
            }
            readersSinceWrite[slot].push_back(node);
        }

        for (const RegRef &reg : access.writes)
        {
            const size_t slot = registerSlot(reg);
            for (size_t reader : readersSinceWrite[slot])
            {
                if (reader != node)
                {
                    addEdge(graph, reader, node, DependencyKind::RegisterWar);
                }
            }
            if (lastWriter[slot] && *lastWriter[slot] != node)
            {
                addEdge(graph, *lastWriter[slot], node, DependencyKind::RegisterWaw);
            }
            lastWriter[slot] = node;
            readersSinceWrite[slot].clear();
        }

        for (const MemoryAccess &current : access.memory)
        {
            for (const MemoryAccess &previous : previousAccesses)
            {
                if (previous.node == node || (!previous.isStore && !current.isStore) ||
                    provablyDisjoint(previous, current))
                {
                    continue;
                }
                DependencyKind kind = DependencyKind::MemoryWaw;
                if (previous.isStore && !current.isStore)
                {
                    kind = DependencyKind::MemoryRaw;
                }
                else if (!previous.isStore && current.isStore)
                {
                    kind = DependencyKind::MemoryWar;
                }
                addEdge(graph, previous.node, node, kind);
            }
        }
        previousAccesses.insert(previousAccesses.end(), access.memory.begin(),
                                access.memory.end());

        // A pinned unit is ordered against every earlier unit; every later unit is ordered
        // against the most recent pinned one (earlier pinned units are reached through it).
        if (isPinned(unit, block))
        {
            for (size_t earlier = 0; earlier < node; ++earlier)
            {
                addEdge(graph, earlier, node, DependencyKind::Fence);
            }
            lastFence = node;
        }
        else if (lastFence)
        {
            addEdge(graph, *lastFence, node, DependencyKind::Fence);
        }
    }

    // The branch or jump that ends the block has to stay last.
    if (block.hasTerminator && block.unitCount > 0)
    {
        const size_t terminator = block.unitCount - 1;
        for (size_t earlier = 0; earlier < terminator; ++earlier)
        {
            addEdge(graph, earlier, terminator, DependencyKind::Control);
        }
    }

    return graph;
}

} // namespace optimizer
} // namespace Kites
