#pragma once

/// @file Collab/SequentialWorkflow.hpp
/// @brief 协作模式 §二.6：Sequential Workflow（流水线，转移表驱动）(Docs/01 §5.6)。
///
/// @details 阶段串行执行，前一阶段产出作为后一阶段输入；任一阶段缺失则短路报错。
///          可选 `on_fail` 钩子（编译期注入 `std::function` 回调非热路径）。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <cstdint>
#include <functional>
#include <generator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Collab/ICollaboration.hpp"
#include "Collab/AgentRuntime.hpp"
#include "Collab/AgentRegistry.hpp"
#include "Collab/MessageBus.hpp"

namespace AE::Engine
{

    struct SequentialWorkflowConfig
    {
        std::vector<std::string> stage_ids;
        // 返回 true 继续，false 中止（可选）。
        std::function<bool(const std::string &stage_id, const std::string &output)> on_fail;
    };

    class SequentialWorkflow final : public ICollaboration
    {
    public:
        explicit SequentialWorkflow(SequentialWorkflowConfig cfg = {}) : m_cfg(std::move(cfg)) {}

        [[nodiscard]] auto name() const -> std::string_view override { return "sequential_workflow"; }

        auto run(CollabContext &ctx) -> std::generator<CollaborationEvent> override
        {
            co_yield CollaborationEvent::collab_started(std::string{name()});

            std::string data = ctx.task.is_string() ? ctx.task.get<std::string>() : ctx.task.dump();
            std::string prev = "human";

            for (const auto &id : m_cfg.stage_ids)
            {
                IAgentRuntime *stage = ctx.registry.get(id);
                if (!stage)
                {
                    co_yield CollaborationEvent::collab_error("stage not found: " + id);
                    co_yield CollaborationEvent::collab_finished(data);
                    co_return;
                }

                AgentMessage msg;
                msg.from = prev;
                msg.to = id;
                msg.kind = AgentMessage::Kind::Handoff;
                msg.content = data;
                ctx.bus.send(msg);
                co_yield CollaborationEvent::message_sent(msg);

                std::string out;
                for (auto &&ce : drive_agent(*stage, msg, ctx, out))
                    co_yield ce;

                if (m_cfg.on_fail && !m_cfg.on_fail(id, out))
                {
                    co_yield CollaborationEvent::collab_error("aborted at stage: " + id);
                    co_yield CollaborationEvent::collab_finished(out);
                    co_return;
                }

                data = out;
                prev = id;
            }

            co_yield CollaborationEvent::consensus(data);
            co_yield CollaborationEvent::collab_finished(data);
        }

    private:
        SequentialWorkflowConfig m_cfg;
    };

} // namespace AE::Engine
