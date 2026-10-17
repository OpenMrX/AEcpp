#pragma once

/// @file Collab/OrchestratorWorker.hpp
/// @brief 协作模式 §二.1：Orchestrator-Worker（控制最集中）(Docs/01 §5.1)。
///
/// @details Orchestrator 产出 `Plan` → 按 `depends_on` 逐步分配到 Worker →
///          聚合各步产出。复用 `Mode/Plan.hpp` 的 DAG 与 `ITaskAssigner`（§5.8）。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <cstdint>
#include <generator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "AEType.hpp" // SelfHealingJson

#include "Mode/Plan.hpp"
#include "Collab/ICollaboration.hpp"
#include "Collab/AgentRuntime.hpp"
#include "Collab/AgentRegistry.hpp"
#include "Collab/MessageBus.hpp"
#include "Collab/TaskAssigner.hpp"

namespace AE::Engine
{

    struct OrchestratorWorkerConfig
    {
        std::string orchestrator_id;
        std::uint32_t max_steps = 8;
    };

    class OrchestratorWorker final : public ICollaboration
    {
    public:
        explicit OrchestratorWorker(OrchestratorWorkerConfig cfg = {},
                                    std::unique_ptr<ITaskAssigner> assigner = nullptr)
            : m_cfg(std::move(cfg)),
              m_assigner(assigner ? std::move(assigner) : std::make_unique<CapabilityAssigner>()) {}

        [[nodiscard]] auto name() const -> std::string_view override { return "orchestrator_worker"; }

        auto run(CollabContext &ctx) -> std::generator<CollaborationEvent> override
        {
            co_yield CollaborationEvent::collab_started(std::string{name()});
            IAgentRuntime *orch = ctx.registry.get(m_cfg.orchestrator_id);
            if (!orch)
            {
                co_yield CollaborationEvent::collab_error("orchestrator not found: " + m_cfg.orchestrator_id);
                co_yield CollaborationEvent::collab_finished();
                co_return;
            }

            // 1) 规划：Orchestrator 产出 JSON 计划。
            AgentMessage task_msg;
            task_msg.from = "human";
            task_msg.to = orch->spec().id;
            task_msg.kind = AgentMessage::Kind::Task;
            task_msg.content = ctx.task.is_string() ? ctx.task.get<std::string>() : ctx.task.dump();
            ctx.bus.send(task_msg);
            co_yield CollaborationEvent::message_sent(task_msg);

            std::string plan_text;
            for (auto &&ce : drive_agent(*orch, task_msg, ctx, plan_text))
                co_yield ce;

            Plan plan = parse_plan(plan_text);
            std::string why;
            if (!plan.validate(&why))
            {
                co_yield CollaborationEvent::collab_error("invalid plan: " + why);
                co_yield CollaborationEvent::collab_finished();
                co_return;
            }

            // 2) 逐步执行（尊重 depends_on）。
            std::string aggregated;
            std::uint32_t executed = 0;
            while (PlanStep *ready = plan.pick_ready())
            {
                if (executed++ >= m_cfg.max_steps)
                    break;
                auto wid = m_assigner->assign(*ready, ctx.registry);
                IAgentRuntime *w = wid.isSuccess() ? ctx.registry.get(wid.value()) : nullptr;
                if (!w)
                {
                    co_yield CollaborationEvent::collab_error(
                        wid.isError() ? wid.error().message() : ("worker not found: " + wid.value()));
                    ready->status = PlanStatus::Failed;
                    continue;
                }

                AgentMessage step_msg;
                step_msg.from = orch->spec().id;
                step_msg.to = w->spec().id;
                step_msg.kind = AgentMessage::Kind::Task;
                step_msg.content = ready->description;
                step_msg.data = ready->to_json();
                ctx.bus.send(step_msg);
                co_yield CollaborationEvent::message_sent(step_msg);

                std::string out;
                for (auto &&ce : drive_agent(*w, step_msg, ctx, out))
                    co_yield ce;

                ready->result = out;
                ready->status = PlanStatus::Done;

                AgentMessage res;
                res.from = w->spec().id;
                res.to = orch->spec().id;
                res.kind = AgentMessage::Kind::Result;
                res.content = out;
                res.reply_to = step_msg.id;
                ctx.bus.send(res);
                co_yield CollaborationEvent::message_sent(res);
            }

            // 3) 聚合各步产出。
            for (const auto &s : plan.steps)
                if (!s.result.empty())
                    aggregated += "[step " + std::to_string(s.id) + "] " + s.result + "\n";
            co_yield CollaborationEvent::consensus(aggregated);
            co_yield CollaborationEvent::collab_finished(aggregated);
        }

    private:
        static auto parse_plan(const std::string &text) -> Plan
        {
            auto [j, level] = SelfHealingJson::repairWithFallback(text);
            (void)level;
            if (!j.has_value() || !j->is_object())
                return {};
            return Plan::from_json(*j);
        }

        OrchestratorWorkerConfig m_cfg;
        std::unique_ptr<ITaskAssigner> m_assigner;
    };

} // namespace AE::Engine
