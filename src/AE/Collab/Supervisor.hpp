#pragma once

/// @file Collab/Supervisor.hpp
/// @brief 协作模式 §二.2：Supervisor（持续质量闸门）(Docs/01 §5.2)。
///
/// @details 在 Orchestrator-Worker 之上叠加对每个 `Result` 的评审：不达标则把
///          `feedback` 回传子 Agent 修订（≤ max_revise），复用 `Critique`（§4.4）。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <cstdint>
#include <generator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Logger.hpp"

#include "Collab/ICollaboration.hpp"
#include "Collab/AgentRuntime.hpp"
#include "Collab/AgentRegistry.hpp"
#include "Collab/MessageBus.hpp"
#include "Collab/Critic.hpp"

namespace AE::Engine
{

    struct SupervisorConfig
    {
        std::string supervisor_id;
        std::vector<std::string> worker_ids; // 空 = 注册表全部（排除 supervisor）
        std::string criteria;
        std::uint32_t max_revise = 2;
    };

    class Supervisor final : public ICollaboration
    {
    public:
        explicit Supervisor(SupervisorConfig cfg = {},
                            std::unique_ptr<ISubmissionCritic> critic = nullptr)
            : m_cfg(std::move(cfg)),
              m_critic(critic ? std::move(critic) : std::make_unique<PassThroughCritic>()) {}

        [[nodiscard]] auto name() const -> std::string_view override { return "supervisor"; }

        auto run(CollabContext &ctx) -> std::generator<CollaborationEvent> override
        {
            co_yield CollaborationEvent::collab_started(std::string{name()});
            const std::string task_text = ctx.task.is_string() ? ctx.task.get<std::string>() : ctx.task.dump();

            std::vector<std::string> workers = m_cfg.worker_ids;
            if (workers.empty())
                for (auto *a : ctx.registry.all())
                    if (a->spec().id != m_cfg.supervisor_id)
                        workers.push_back(a->spec().id);

            std::string aggregated;
            for (const auto &id : workers)
            {
                IAgentRuntime *w = ctx.registry.get(id);
                if (!w)
                {
                    co_yield CollaborationEvent::collab_error("worker not found: " + id);
                    continue;
                }

                AgentMessage task;
                task.from = m_cfg.supervisor_id.empty() ? "supervisor" : m_cfg.supervisor_id;
                task.to = id;
                task.kind = AgentMessage::Kind::Task;
                task.content = task_text;
                ctx.bus.send(task);
                co_yield CollaborationEvent::message_sent(task);

                std::uint32_t revise = 0;
                std::string out;
                for (;;)
                {
                    for (auto &&ce : drive_agent(*w, task, ctx, out))
                        co_yield ce;

                    auto cr = m_critic->review(out, m_cfg.criteria, ctx);
                    if (cr.isError())
                    {
                        co_yield CollaborationEvent::collab_error(cr.error().message());
                        break;
                    }
                    const Critique &c = cr.value();
                    co_yield CollaborationEvent::vote(id, std::string{"score="} + std::to_string(c.score));

                    if (c.passed || revise >= m_cfg.max_revise)
                    {
                        aggregated += out;
                        aggregated += "\n";
                        break;
                    }

                    ++revise;
                    AgentMessage rev;
                    rev.from = task.from;
                    rev.to = id;
                    rev.kind = AgentMessage::Kind::Task;
                    rev.content = "[修订要求] " + (c.feedback.empty() && !c.issues.empty() ? c.issues.front() : c.feedback);
                    rev.reply_to = task.id;
                    rev.data = c.to_json();
                    ctx.bus.send(rev);
                    co_yield CollaborationEvent::message_sent(rev);
                    task = rev;
                }
            }

            co_yield CollaborationEvent::consensus(aggregated);
            co_yield CollaborationEvent::collab_finished(aggregated);
        }

    private:
        SupervisorConfig m_cfg;
        std::unique_ptr<ISubmissionCritic> m_critic;
    };

} // namespace AE::Engine
