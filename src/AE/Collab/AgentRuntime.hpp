#pragma once

/// @file Collab/AgentRuntime.hpp
/// @brief 单 Agent 实例：配置（AgentSpec）+ 模式 + 共享依赖（Docs/01 §5.0.3、§3.4）。
///
/// @details
/// - `IAgentRuntime` 为协作层调用单 Agent 的边界抽象（每消息一次）。
/// - 具体 `AgentRuntime` 内部持有 `std::variant` 模式（§3.4 零开销：入口一次 visit，
///   循环内部静态内联），按 `AgentSpec::mode` 选择 ReAct / Plan-Execute / Reflection。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <cstddef>
#include <cstdint>
#include <generator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "nlohmann-json.hpp"

#include "AEType.hpp"
#include "LLMAdapter.hpp"
#include "AETelemetry.hpp"

#include "Mode/ModeContext.hpp"
#include "Mode/ReactMode.hpp"
#include "Mode/PlanExecuteMode.hpp"
#include "Mode/IReflector.hpp"
#include "Mode/ReflectionMode.hpp"

#include "Collab/AgentSpec.hpp"
#include "Collab/CollaborationEvent.hpp"
#include "Collab/ICollaboration.hpp"

namespace AE::Engine
{

    class IAgentRuntime
    {
    public:
        virtual ~IAgentRuntime() = default;
        [[nodiscard]] virtual auto spec() const -> const AgentSpec & = 0;
        virtual auto run(const AgentMessage &in, CollabContext &ctx) -> std::generator<AgentEvent> = 0;
    };

    // ---------------------------------------------------- 具体运行时（模式驱动）
    class AgentRuntime final : public IAgentRuntime
    {
    public:
        AgentRuntime(AgentSpec spec,
                     IToolProvider &tools,
                     IContextSource &ctx_source,
                     LLMAdapter &llm,
                     IPermissionGate *gate = nullptr,
                     IStateStore *store = nullptr,
                     std::shared_ptr<AETelemetry> telemetry = nullptr,
                     std::shared_ptr<IReflector> reflector = nullptr,
                     PlanExecuteConfig plan_cfg = {},
                     ReflectionConfig reflect_cfg = {},
                     std::size_t max_iterations = 16)
            : m_spec(std::move(spec)),
              m_tools(tools), m_ctx_source(ctx_source), m_llm(llm),
              m_gate(gate), m_store(store), m_tele(std::move(telemetry)),
              m_reflector(reflector ? std::move(reflector) : std::make_shared<RuleReflector>()),
              m_plan_cfg(plan_cfg), m_reflect_cfg(reflect_cfg), m_max_iter(max_iterations)
        {
            switch (m_spec.mode)
            {
            case ModeKind::PlanExecute:
                m_mode.template emplace<PlanExecuteMode>(m_plan_cfg);
                break;
            case ModeKind::Reflection:
                m_mode.template emplace<ReflectionMode<ReactMode>>(
                    ReactMode{}, *m_reflector, m_reflect_cfg);
                break;
            case ModeKind::React:
            default:
                m_mode.template emplace<ReactMode>();
                break;
            }
        }

        [[nodiscard]] auto spec() const -> const AgentSpec & override { return m_spec; }

        auto run(const AgentMessage &in, CollabContext &ctx) -> std::generator<AgentEvent> override
        {
            ModeContext mc{m_tools, m_ctx_source, m_llm, m_gate, m_store, m_tele};
            mc.human = ctx.human;
            mc.request_body = build_body(in);
            mc.begin_run(ctx.session_id + "/" + m_spec.id, mc.request_body,
                         static_cast<std::uint32_t>(m_max_iter));
            for (auto &&e : std::visit([&](auto &m) { return m.execute(mc); }, m_mode))
                co_yield std::move(e);
        }

    private:
        [[nodiscard]] auto build_body(const AgentMessage &in) const -> nlohmann::json
        {
            nlohmann::json msgs = nlohmann::json::array();
            if (!m_spec.system_prompt.empty())
                msgs.push_back(nlohmann::json::object(
                    {{"role", "system"}, {"content", m_spec.system_prompt}}));
            msgs.push_back(nlohmann::json::object({{"role", "user"}, {"content", in.content}}));
            nlohmann::json body = nlohmann::json::object();
            body["messages"] = std::move(msgs);
            if (!m_spec.model.empty())
                body["model"] = m_spec.model;
            return body;
        }

        using RuntimeMode = std::variant<ReactMode, PlanExecuteMode, ReflectionMode<ReactMode>>;

        AgentSpec m_spec;
        IToolProvider &m_tools;
        IContextSource &m_ctx_source;
        LLMAdapter &m_llm;
        IPermissionGate *m_gate;
        IStateStore *m_store;
        std::shared_ptr<AETelemetry> m_tele;
        std::shared_ptr<IReflector> m_reflector;
        PlanExecuteConfig m_plan_cfg;
        ReflectionConfig m_reflect_cfg;
        std::size_t m_max_iter;
        RuntimeMode m_mode{std::in_place_type<ReactMode>};
    };

    // ---------------------------------------------------- 运行助手
    // 运行单个子 Agent：发出 AgentStarted → 透传内层事件 → AgentFinished(终答写入 out)。
    inline auto drive_agent(IAgentRuntime &rt, const AgentMessage &in, CollabContext &ctx, std::string &out)
        -> std::generator<CollaborationEvent>
    {
        const std::string id = rt.spec().id;
        co_yield CollaborationEvent::agent_started(id);
        out.clear();
        for (auto &&e : rt.run(in, ctx))
        {
            if (e.kind == EventKind::Final)
                out = e.payload;
            co_yield CollaborationEvent::inner_event(id, std::move(e));
        }
        co_yield CollaborationEvent::agent_finished(id, out);
    }

} // namespace AE::Engine
