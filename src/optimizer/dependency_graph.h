/**
 * @file dependency_graph.h
 * @brief The ordering constraints between the units of one basic block.
 *
 * Each node is a unit (see program_model.h); node k is the k-th unit of the block in its original
 * order. An edge `from -> to` means `from` must stay before `to` in any legal reordering. A
 * reordering is legal exactly when it respects every edge, so a scheduler only has to pick any
 * order that does.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "optimizer/program_model.h"

namespace Kites
{
namespace optimizer
{
enum class DependencyKind : uint8_t
{
    RegisterRaw, ///< read-after-write: `to` reads a register `from` writes (true dependency)
    RegisterWar, ///< write-after-read: `to` overwrites a register `from` still needs to read
    RegisterWaw, ///< write-after-write: both write the same register
    MemoryRaw,   ///< a load after a store that may touch the same bytes
    MemoryWar,   ///< a store after a load that may touch the same bytes
    MemoryWaw,   ///< two stores that may touch the same bytes
    Fence,       ///< ordered against an instruction nothing may move across
    Control      ///< must stay before the block's branch/jump
};

struct Dependency
{
    size_t from{0};
    size_t to{0};
    DependencyKind kind{DependencyKind::RegisterRaw};
};

struct DependencyGraph
{
    size_t nodeCount{0};
    std::vector<Dependency> edges;

    /// Indices into `edges`, per node.
    std::vector<std::vector<size_t>> incoming;
    std::vector<std::vector<size_t>> outgoing;

    /**
     * @brief Checks that `order` is a legal reordering.
     * @param order order[k] is the node placed at position k. It must be a permutation of all
     * nodes, and every edge must go from an earlier position to a later one.
     */
    [[nodiscard]] bool isRespectedBy(const std::vector<size_t> &order) const;
};

/**
 * @brief Builds the dependency graph of one basic block.
 *
 * Register dependencies are tracked per register file, so x5 and f5 are independent, and x0 never
 * creates a dependency. Two memory accesses are only treated as independent when they use the same
 * base register (unchanged between the accesses) and their byte ranges cannot overlap; anything
 * else is assumed to alias. Units that must not move (barriers, units that read their own
 * address, and every unit of a non-reorderable block) are fences. The block's terminator stays
 * last.
 */
[[nodiscard]] DependencyGraph buildDependencyGraph(const ProgramModel &model,
                                                   const BasicBlock &block);

} // namespace optimizer
} // namespace Kites
