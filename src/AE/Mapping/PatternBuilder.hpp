#pragma once

/// @file Mapping/PatternBuilder.hpp
/// @brief 用原语/直接构造拼装 6 种协作模式（Docs/01 §6.3、§6.6）。
///
/// @details 每个 Builder 返回 `ICollaboration`，与「直接构造」（§5）产出等价事件序列。
///          传入 `AgentRegistry` 用于在构建期以注册表填充缺省 id（运行期仍经
///          `CollabContext::registry` 解析，保证与 §5 一致）。
///
/// @note 命名空间为扁平 `AE::Engine` + 构建器分组 `Patterns`（评审结论 #1）。

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Collab/ICollaboration.hpp"
#include "Collab/AgentRegistry.hpp"
#include "Collab/OrchestratorWorker.hpp"
#include "Collab/Supervisor.hpp"
#include "Collab/GroupChat.hpp"
#include "Collab/Debate.hpp"
#include "Collab/PeerToPeer.hpp"
#include "Collab/SequentialWorkflow.hpp"

namespace AE::Engine::Patterns
{

    namespace detail
    {
        [[nodiscard]] inline auto all_ids(const AgentRegistry &reg) -> std::vector<std::string>
        {
            std::vector<std::string> ids;
            for (auto *a : reg.all())
                ids.push_back(a->spec().id);
            return ids;
        }
    } // namespace detail

    [[nodiscard]] inline auto OrchestratorWorker(const AgentRegistry &reg, OrchestratorWorkerConfig cfg)
        -> std::unique_ptr<ICollaboration>
    {
        if (cfg.orchestrator_id.empty())
        {
            auto ids = detail::all_ids(reg);
            if (!ids.empty())
                cfg.orchestrator_id = ids.front();
        }
        return std::make_unique<Engine::OrchestratorWorker>(std::move(cfg));
    }

    [[nodiscard]] inline auto Supervisor(const AgentRegistry &reg, SupervisorConfig cfg)
        -> std::unique_ptr<ICollaboration>
    {
        if (cfg.worker_ids.empty())
            cfg.worker_ids = detail::all_ids(reg);
        return std::make_unique<Engine::Supervisor>(std::move(cfg));
    }

    [[nodiscard]] inline auto GroupChat(const AgentRegistry &reg, GroupChatConfig cfg)
        -> std::unique_ptr<ICollaboration>
    {
        if (cfg.participant_ids.empty())
            cfg.participant_ids = detail::all_ids(reg);
        return std::make_unique<Engine::GroupChat>(std::move(cfg));
    }

    [[nodiscard]] inline auto Debate(const AgentRegistry &reg, DebateConfig cfg)
        -> std::unique_ptr<ICollaboration>
    {
        if (cfg.debater_ids.empty())
            cfg.debater_ids = detail::all_ids(reg);
        return std::make_unique<Engine::Debate>(std::move(cfg));
    }

    [[nodiscard]] inline auto PeerToPeer(const AgentRegistry &reg, PeerToPeerConfig cfg)
        -> std::unique_ptr<ICollaboration>
    {
        if (cfg.local_id.empty())
        {
            auto ids = detail::all_ids(reg);
            if (!ids.empty())
                cfg.local_id = ids.front();
        }
        return std::make_unique<Engine::PeerToPeer>(std::move(cfg));
    }

    [[nodiscard]] inline auto Sequential(const AgentRegistry &reg, std::vector<std::string> stages)
        -> std::unique_ptr<ICollaboration>
    {
        if (stages.empty())
            stages = detail::all_ids(reg);
        return std::make_unique<Engine::SequentialWorkflow>(
            SequentialWorkflowConfig{.stage_ids = std::move(stages)});
    }

} // namespace AE::Engine::Patterns
