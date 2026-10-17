#pragma once

/// @file Mapping/Primitives/Graph.hpp
/// @brief 图原语（LangGraph 隐喻，Docs/01 §6.2、§6.5）。
///
/// @details `Node`/`Edge`/条件边 + 检查点；`compile()` 产出 `ICollaboration`，
///          运行时按节点函数串联执行、条件边路由、可选检查点落库。
///
/// @note 命名空间为扁平 `AE::Engine`（评审结论 #1）；文档 §6.5 中的
///       `AE::Engine::Mapping` 前缀按该结论去除。

#include <cstddef>
#include <functional>
#include <generator>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Error.hpp"

#include "AEType.hpp" // IStateStore

#include "Collab/ICollaboration.hpp"
#include "Collab/MessageBus.hpp"
#include "Collab/CollaborationEvent.hpp"

namespace AE::Engine
{

    // 节点函数：输入当前状态 JSON，输出下一状态 JSON。
    using GraphFn = std::function<Core::Result<nlohmann::json>(const nlohmann::json &, CollabContext &)>;

    struct GraphNode
    {
        std::string id;
        GraphFn fn;
    };

    struct GraphEdge
    {
        std::string from;
        std::string to;
        std::function<bool(const nlohmann::json &)> guard; // 空 = 无条件
    };

    class Graph
    {
    public:
        auto add_node(GraphNode n) -> Graph &
        {
            m_nodes.push_back(std::move(n));
            return *this;
        }

        auto add_edge(GraphEdge e) -> Graph &
        {
            m_edges.push_back(std::move(e));
            return *this;
        }

        // 条件边：由 `router` 依据状态返回下一节点 id（空 = 终止）。
        auto add_conditional(std::string from,
                             std::function<std::string(const nlohmann::json &)> router) -> Graph &
        {
            m_cond[std::move(from)] = std::move(router);
            return *this;
        }

        auto set_checkpoint(IStateStore &s) -> Graph &
        {
            m_checkpoint = &s;
            return *this;
        }

        auto set_start(std::string id) -> Graph &
        {
            m_start = std::move(id);
            return *this;
        }

        [[nodiscard]] auto node(std::string_view id) const -> const GraphNode *
        {
            for (const auto &n : m_nodes)
                if (n.id == id)
                    return &n;
            return nullptr;
        }

        [[nodiscard]] auto nodes() const -> const std::vector<GraphNode> & { return m_nodes; }
        [[nodiscard]] auto edges() const -> const std::vector<GraphEdge> & { return m_edges; }
        [[nodiscard]] auto has_conditional(std::string_view from) const -> bool
        {
            return m_cond.find(std::string{from}) != m_cond.end();
        }

        // 编译为协作运行体。
        [[nodiscard]] auto compile() const -> std::unique_ptr<ICollaboration>;

    private:
        friend class GraphRun;
        [[nodiscard]] auto start_id() const -> std::string;
        [[nodiscard]] auto next_id(std::string_view cur, const nlohmann::json &state) const -> std::string;

        std::vector<GraphNode> m_nodes;
        std::vector<GraphEdge> m_edges;
        std::unordered_map<std::string, std::function<std::string(const nlohmann::json &)>> m_cond;
        IStateStore *m_checkpoint{nullptr};
        std::string m_start;
    };

    // ---------------------------------------------------- 图运行体
    class GraphRun final : public ICollaboration
    {
    public:
        explicit GraphRun(const Graph &g) : m_graph(g) {}

        [[nodiscard]] auto name() const -> std::string_view override { return "graph"; }

        auto run(CollabContext &ctx) -> std::generator<CollaborationEvent> override
        {
            co_yield CollaborationEvent::collab_started("graph");
            if (m_graph.m_nodes.empty())
            {
                co_yield CollaborationEvent::collab_error("graph has no nodes");
                co_yield CollaborationEvent::collab_finished();
                co_return;
            }

            std::string cur = m_graph.start_id();
            nlohmann::json state = ctx.task;
            const std::size_t limit = m_graph.m_nodes.size() * 8 + 8;
            for (std::size_t step = 0; step < limit && !cur.empty(); ++step)
            {
                const GraphNode *n = m_graph.node(cur);
                if (!n)
                {
                    co_yield CollaborationEvent::collab_error("node not found: " + cur);
                    break;
                }

                co_yield CollaborationEvent::agent_started(cur);
                auto r = n->fn(state, ctx);
                if (r.isError())
                {
                    co_yield CollaborationEvent::collab_error(r.error().message());
                    break;
                }
                state = std::move(r.value());

                AgentMessage m;
                m.from = cur;
                m.to = "*";
                m.kind = AgentMessage::Kind::Info;
                m.content = state.dump();
                ctx.bus.send(m);
                co_yield CollaborationEvent::message_sent(m);
                co_yield CollaborationEvent::agent_finished(cur, state.dump());

                if (m_graph.m_checkpoint)
                    (void)m_graph.m_checkpoint->save(ctx.session_id, state.dump());

                cur = m_graph.next_id(cur, state);
            }

            co_yield CollaborationEvent::consensus(state.dump());
            co_yield CollaborationEvent::collab_finished(state.dump());
        }

    private:
        const Graph m_graph;
    };

    inline auto Graph::start_id() const -> std::string
    {
        if (!m_start.empty())
            return m_start;
        // 默认起点：首个无入边的节点，否则首个节点。
        for (const auto &n : m_nodes)
        {
            bool has_in = false;
            for (const auto &e : m_edges)
                if (e.to == n.id)
                {
                    has_in = true;
                    break;
                }
            if (!has_in)
                return n.id;
        }
        return m_nodes.empty() ? std::string{} : m_nodes.front().id;
    }

    inline auto Graph::next_id(std::string_view cur, const nlohmann::json &state) const -> std::string
    {
        if (auto it = m_cond.find(std::string{cur}); it != m_cond.end())
            return it->second(state);
        for (const auto &e : m_edges)
        {
            if (e.from != cur)
                continue;
            if (!e.guard || e.guard(state))
                return e.to;
        }
        return {};
    }

    inline auto Graph::compile() const -> std::unique_ptr<ICollaboration>
    {
        return std::make_unique<GraphRun>(*this);
    }

} // namespace AE::Engine
