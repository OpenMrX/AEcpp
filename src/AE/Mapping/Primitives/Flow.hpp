#pragma once

/// @file Mapping/Primitives/Flow.hpp
/// @brief 强类型数据流工作流原语（MS Agent Framework 隐喻，Docs/01 §6.2、§6.5）。
///
/// @details 节点函数为纯 `json -> Result<json>`（无外部上下文），适合确定性数据处理；
///          `compile()` 将其包装为 `ICollaboration`（事件外发到 `MessageBus`）。
///
/// @note 命名空间为扁平 `AE::Engine`（评审结论 #1）。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <generator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Error.hpp"

#include "Collab/ICollaboration.hpp"
#include "Collab/MessageBus.hpp"
#include "Collab/CollaborationEvent.hpp"

namespace AE::Engine
{

    struct FlowNode
    {
        std::string id;
        std::function<Core::Result<nlohmann::json>(const nlohmann::json &)> fn;
    };

    struct FlowEdge
    {
        std::string from;
        std::string to;
        std::function<bool(const nlohmann::json &)> guard; // 空 = 无条件
    };

    class FlowGraph
    {
    public:
        auto add_node(FlowNode n) -> FlowGraph &
        {
            m_nodes.push_back(std::move(n));
            return *this;
        }

        auto add_edge(std::string from, std::string to) -> FlowGraph &
        {
            m_edges.push_back(FlowEdge{std::move(from), std::move(to), {}});
            return *this;
        }

        auto add_edge(FlowEdge e) -> FlowGraph &
        {
            m_edges.push_back(std::move(e));
            return *this;
        }

        auto set_entry(std::string id) -> FlowGraph &
        {
            m_entry = std::move(id);
            return *this;
        }

        [[nodiscard]] auto nodes() const -> const std::vector<FlowNode> & { return m_nodes; }
        [[nodiscard]] auto edges() const -> const std::vector<FlowEdge> & { return m_edges; }

        // 同步执行（无外部上下文）。
        [[nodiscard]] auto run(nlohmann::json input) const -> Core::Result<nlohmann::json>
        {
            if (m_nodes.empty())
                return Core::Result<nlohmann::json>::Error(
                    Core::Error(Core::ErrorCode::InvalidArgument, "flow has no nodes"));

            std::string cur = entry_id();
            nlohmann::json state = std::move(input);
            const std::size_t limit = m_nodes.size() * 8 + 8;
            for (std::size_t step = 0; step < limit && !cur.empty(); ++step)
            {
                const FlowNode *n = find(cur);
                if (!n)
                    return Core::Result<nlohmann::json>::Error(
                        Core::Error(Core::ErrorCode::NotFound, "flow node not found: " + cur));
                auto r = n->fn(state);
                if (r.isError())
                    return Core::Result<nlohmann::json>::Error(r.error());
                state = std::move(r.value());
                cur = next(cur, state);
            }
            return Core::Result<nlohmann::json>::Ok(std::move(state));
        }

        [[nodiscard]] auto compile() const -> std::unique_ptr<ICollaboration>;

    private:
        friend class FlowRun;
        [[nodiscard]] auto entry_id() const -> std::string
        {
            if (!m_entry.empty())
                return m_entry;
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

        [[nodiscard]] auto find(std::string_view id) const -> const FlowNode *
        {
            for (const auto &n : m_nodes)
                if (n.id == id)
                    return &n;
            return nullptr;
        }

        [[nodiscard]] auto next(std::string_view cur, const nlohmann::json &state) const -> std::string
        {
            for (const auto &e : m_edges)
            {
                if (e.from != cur)
                    continue;
                if (!e.guard || e.guard(state))
                    return e.to;
            }
            return {};
        }

        std::vector<FlowNode> m_nodes;
        std::vector<FlowEdge> m_edges;
        std::string m_entry;
    };

    class FlowRun final : public ICollaboration
    {
    public:
        explicit FlowRun(FlowGraph g) : m_flow(std::move(g)) {}

        [[nodiscard]] auto name() const -> std::string_view override { return "flow"; }

        auto run(CollabContext &ctx) -> std::generator<CollaborationEvent> override
        {
            co_yield CollaborationEvent::collab_started("flow");
            auto r = m_flow.run(ctx.task);
            if (r.isError())
            {
                co_yield CollaborationEvent::collab_error(r.error().message());
                co_yield CollaborationEvent::collab_finished();
                co_return;
            }
            AgentMessage m;
            m.from = "flow";
            m.to = "*";
            m.kind = AgentMessage::Kind::Result;
            m.content = r.value().dump();
            ctx.bus.send(m);
            co_yield CollaborationEvent::message_sent(m);
            co_yield CollaborationEvent::consensus(m.content);
            co_yield CollaborationEvent::collab_finished(m.content);
        }

    private:
        FlowGraph m_flow;
    };

    inline auto FlowGraph::compile() const -> std::unique_ptr<ICollaboration>
    {
        return std::make_unique<FlowRun>(*this);
    }

} // namespace AE::Engine
