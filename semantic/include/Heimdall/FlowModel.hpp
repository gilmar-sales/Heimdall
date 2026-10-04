#pragma once

#include <Heimdall/TypeModel.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <vector>

namespace heimdall
{

    // F4 of docs/semantic-engine-architecture.md: an intraprocedural control-flow
    // graph and def-use chains for every function body and lambda of a bound,
    // typed file. Same conventions as the other layers: 32-bit indices into
    // structure-of-arrays tables, one arena, immutable once built.
    using FunctionId = std::uint32_t;
    using BlockId = std::uint32_t;

    // What one occurrence of a local variable or parameter does to it. Inside a
    // statement, events are in token order (not evaluation order).
    enum class EventKind : std::uint8_t
    {
        Init,   // declared with an initializer
        Uninit, // declared without one
        Read,   // its value is read; the object is not touched
        Write,  // `x = e`: replaced as a whole
        Modify, // `++x`, `x += e`, `x[i] = e` on an array, `x.m = e`, a non-const member call...
        Escape  // the engine cannot tell what happens to it: address taken, bound to a
                // non-const reference, passed to a callee it does not know, captured by a
                // lambda, or named somewhere the grammar did not model
    };

    // Function bodies and lambda bodies: one entry per function-like node.
    struct FunctionTable
    {
        explicit FunctionTable(std::pmr::memory_resource *resource);

        std::pmr::vector<std::uint32_t> node;   // FunctionDefinition or LambdaExpression
        std::pmr::vector<std::uint32_t> body;   // its CompoundStatement
        std::pmr::vector<SymbolId> symbol;      // the function symbol, kNone for lambdas
        std::pmr::vector<BlockId> entry;
        std::pmr::vector<BlockId> exit;         // every `return` (and the fall off the end) reaches it
        std::pmr::vector<BlockId> first_block;  // blocks of one function are contiguous
        std::pmr::vector<std::uint32_t> block_count;
        // False when something the CFG cannot model is present (`goto`, `asm`,
        // coroutines, error nodes, nesting deeper than the engine follows): every
        // rule must stay silent about such a function.
        std::pmr::vector<std::uint8_t> complete;

        std::size_t Size() const noexcept
        {
            return node.size();
        }
    };

    // Basic blocks: straight-line runs of events with edges to their successors.
    struct BlockTable
    {
        explicit BlockTable(std::pmr::memory_resource *resource);

        std::pmr::vector<FunctionId> function;
        std::pmr::vector<std::uint32_t> first_event; // range in EventTable
        std::pmr::vector<std::uint32_t> event_count;
        std::pmr::vector<std::uint32_t> first_succ;  // range in FlowModel::SuccessorList
        std::pmr::vector<std::uint32_t> succ_count;
        std::pmr::vector<std::uint8_t> returns;      // ends in `return` (or `throw`)

        std::size_t Size() const noexcept
        {
            return function.size();
        }
    };

    struct EventTable
    {
        explicit EventTable(std::pmr::memory_resource *resource);

        std::pmr::vector<EventKind> kind;
        std::pmr::vector<SymbolId> symbol;
        std::pmr::vector<std::uint32_t> token; // index into ParseTree::Tokens()
        std::pmr::vector<BlockId> block;

        std::size_t Size() const noexcept
        {
            return kind.size();
        }
    };

    class FlowModel
    {
    public:
        explicit FlowModel(const TypeModel &types, std::size_t arena_hint = 64 * 1024);
        FlowModel(FlowModel &&) noexcept = default;

        const TypeModel &Types() const noexcept
        {
            return *m_types;
        }
        const SemanticModel &Model() const noexcept
        {
            return m_types->Model();
        }
        const FunctionTable &Functions() const noexcept
        {
            return m_functions;
        }
        const BlockTable &Blocks() const noexcept
        {
            return m_blocks;
        }
        const EventTable &Events() const noexcept
        {
            return m_events;
        }
        std::span<const BlockId> Successors(BlockId block) const noexcept
        {
            if (block >= m_blocks.Size())
            {
                return {};
            }

            return {m_successors.data() + m_blocks.first_succ[block], m_blocks.succ_count[block]};
        }

        // Function or lambda whose body declares the variable or parameter, or
        // kNone for anything not local (globals, members, locals of an unmodeled
        // body).
        FunctionId OwnerOf(SymbolId symbol) const noexcept
        {
            return symbol < m_owner.size() ? m_owner[symbol] : kNone;
        }
        // Function-like node (FunctionDefinition or LambdaExpression) -> its id.
        FunctionId FunctionOfNode(std::uint32_t node) const noexcept;
        // Indices into Events() of everything that happens to `symbol`, in token order.
        std::span<const std::uint32_t> EventsOf(SymbolId symbol) const noexcept
        {
            if (symbol + 1 >= m_symbol_begin.size())
            {
                return {};
            }

            return {m_symbol_events.data() + m_symbol_begin[symbol], m_symbol_begin[symbol + 1] - m_symbol_begin[symbol]};
        }
        // Blocks reachable from the function's entry.
        bool IsReachable(FunctionId function, BlockId block) const;
        // Some `return` (or the end of the body) can be reached from the entry.
        bool ExitReachable(FunctionId function) const;
        // A reachable `return` or `throw` statement exists.
        bool HasReachableReturn(FunctionId function) const;
        // Def-use summary: the variable is declared with an initializer and nothing
        // ever writes, modifies or leaks it. False whenever anything is unknown.
        bool IsNeverModified(SymbolId symbol) const;
        std::size_t ArenaBytes() const noexcept
        {
            return m_arena->Used();
        }

    private:
        friend class FlowBuilder;

        std::vector<std::uint8_t> ReachableSet(FunctionId function) const;

        std::unique_ptr<Arena> m_arena;
        const TypeModel *m_types;
        FunctionTable m_functions;
        BlockTable m_blocks;
        EventTable m_events;
        std::pmr::vector<std::uint32_t> m_successors;
        std::pmr::vector<FunctionId> m_owner;               // per symbol
        std::pmr::vector<std::uint32_t> m_symbol_begin;     // per symbol + 1
        std::pmr::vector<std::uint32_t> m_symbol_events;
        std::pmr::vector<std::uint32_t> m_node_function;    // per node
    };

    // Builds the CFG and the def-use events in one pass over the function nodes.
    class Flow
    {
    public:
        static FlowModel Build(const TypeModel &types);
    };

} // namespace heimdall
