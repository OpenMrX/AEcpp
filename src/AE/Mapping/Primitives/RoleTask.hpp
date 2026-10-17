#pragma once

/// @file Mapping/Primitives/RoleTask.hpp
/// @brief 角色/任务/流程原语（CrewAI 隐喻，Docs/01 §6.2、§6.5）。
///
/// @details `Role`/`Task`/`Process` + `CrewBuilder`：顺序流程委托 `SequentialWorkflow`（§5.6），
///          层级流程委托 `OrchestratorWorker`（§5.1）。
///
/// @note 命名空间为扁平 `AE::Engine`（评审结论 #1）。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Collab/ICollaboration.hpp"
#include "Collab/SequentialWorkflow.hpp"
#include "Collab/OrchestratorWorker.hpp"

namespace AE::Engine
{

    struct Role
    {
        std::string name;
        std::string goal;
        std::string backstory;
        std::string model;
    };

    struct Task
    {
        std::string description;
        std::string expected_output;
        std::optional<std::string> agent; // 指定执行角色（Agent id）；空则按角色顺序
    };

    enum class Process : std::uint8_t
    {
        Sequential,
        Hierarchical
    };

    class CrewBuilder
    {
    public:
        auto add_role(Role r) -> CrewBuilder &
        {
            m_roles.push_back(std::move(r));
            return *this;
        }

        auto add_task(Task t) -> CrewBuilder &
        {
            m_tasks.push_back(std::move(t));
            return *this;
        }

        auto process(Process p) -> CrewBuilder &
        {
            m_process = p;
            return *this;
        }

        [[nodiscard]] auto build() const -> std::unique_ptr<ICollaboration>
        {
            std::vector<std::string> stages;
            for (std::size_t i = 0; i < m_tasks.size(); ++i)
            {
                if (m_tasks[i].agent && !m_tasks[i].agent->empty())
                    stages.push_back(*m_tasks[i].agent);
                else if (i < m_roles.size())
                    stages.push_back(m_roles[i].name);
            }

            if (m_process == Process::Hierarchical && !stages.empty())
            {
                OrchestratorWorkerConfig cfg;
                // 层级：首个角色为编排者（若已显式指定任务 agent，则以其为准）。
                cfg.orchestrator_id = m_roles.empty() ? stages.front() : m_roles.front().name;
                return std::make_unique<OrchestratorWorker>(std::move(cfg));
            }

            return std::make_unique<SequentialWorkflow>(
                SequentialWorkflowConfig{.stage_ids = std::move(stages)});
        }

    private:
        std::vector<Role> m_roles;
        std::vector<Task> m_tasks;
        Process m_process{Process::Sequential};
    };

} // namespace AE::Engine
