#pragma once

/// @file Collab/CollabSelector.hpp
/// @brief 协作策略选择（含规则式选择器）(Docs/01 §5.8)。
///
/// @details 依据任务约束与注册表规模选择协作模式，返回已构建的 `ICollaboration`。
///          选择器仅在协作启动时调用一次（非热路径）。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Collab/ICollaboration.hpp"
#include "Collab/AgentRegistry.hpp"
#include "Collab/TaskAssigner.hpp"
#include "Collab/OrchestratorWorker.hpp"
#include "Collab/Supervisor.hpp"
#include "Collab/GroupChat.hpp"
#include "Collab/Debate.hpp"
#include "Collab/PeerToPeer.hpp"
#include "Collab/SequentialWorkflow.hpp"

namespace AE::Engine
{

    enum class CollabStrategy : std::uint8_t
    {
        OrchestratorWorker,
        Supervisor,
        GroupChat,
        Debate,
        PeerToPeer,
        SequentialWorkflow
    };

    struct CollabConstraints
    {
        bool ordered_pipeline{false}; // 步骤强序 → SequentialWorkflow
        bool needs_quality_gate{false};
        bool needs_consensus{false};
        bool decentralized{false};
        std::uint32_t agent_count{0};
    };

    class ICollaborationSelector
    {
    public:
        virtual ~ICollaborationSelector() = default;
        virtual auto select(const CollabConstraints &c, AgentRegistry &reg)
            -> std::unique_ptr<ICollaboration> = 0;
    };

    // 规则式选择器：优先级流水线 > 质量闸门 > 共识 > 去中心 > 默认编排。
    class HeuristicCollabSelector final : public ICollaborationSelector
    {
    public:
        auto select(const CollabConstraints &c, AgentRegistry &reg)
            -> std::unique_ptr<ICollaboration> override
        {
            const auto ids = collect_ids(reg);

            if (c.ordered_pipeline && ids.size() > 1)
                return std::make_unique<SequentialWorkflow>(
                    SequentialWorkflowConfig{.stage_ids = ids});

            if (c.needs_quality_gate)
                return std::make_unique<Supervisor>(SupervisorConfig{.worker_ids = ids});

            if (c.needs_consensus)
            {
                if (ids.size() <= 3)
                    return std::make_unique<Debate>(DebateConfig{.debater_ids = ids});
                return std::make_unique<GroupChat>(GroupChatConfig{.participant_ids = ids});
            }

            if (c.decentralized)
                return std::make_unique<PeerToPeer>(PeerToPeerConfig{});

            if (ids.size() <= 1)
                return std::make_unique<SequentialWorkflow>(
                    SequentialWorkflowConfig{.stage_ids = ids});

            OrchestratorWorkerConfig ow;
            ow.orchestrator_id = ids.front();
            return std::make_unique<OrchestratorWorker>(std::move(ow));
        }

    private:
        static auto collect_ids(AgentRegistry &reg) -> std::vector<std::string>
        {
            std::vector<std::string> ids;
            for (auto *a : reg.all())
                ids.push_back(a->spec().id);
            return ids;
        }
    };

} // namespace AE::Engine
