#pragma once

#include <cstdint>
#include <limits>

namespace heimdall
{
    // Document ids are never recycled during a Workspace's lifetime. Other ids
    // are local to a document in a particular analysis snapshot.
    using DocumentId = std::uint32_t;
    using NodeId = std::uint32_t;
    using StringId = std::uint32_t;
    using SymbolId = std::uint32_t;
    using ScopeId = std::uint32_t;
    using TypeId = std::uint32_t;
    inline constexpr DocumentId InvalidDocument = std::numeric_limits<DocumentId>::max();
    inline constexpr NodeId InvalidNode = std::numeric_limits<NodeId>::max();
}
