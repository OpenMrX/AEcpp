#pragma once

/// @file Collab/TaskAssigner.hpp
/// @brief 任务→Agent 分配（Docs/01 §5.8）。
///
/// @details 仅依赖 `Mode/Plan.hpp` 与 `AgentRegistry.hpp`；每次分配一次调用（边界）。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <string>
#include <string_view>

#include "Core/Result.hpp"
#include "Core/Error.hpp"

#include "Mode/Plan.hpp"
#include "Collab/AgentRegistry.hpp"

namespace AE::Engine
{

    class ITaskAssigner
    {
    public:
        virtual ~ITaskAssigner() = default;
        virtual auto assign(const PlanStep &step, AgentRegistry &reg) -> Core::Result<std::string> = 0;
    };

    // 默认：按 step.capability 选首个匹配 agent；无 capability 则取注册表首个。
    class CapabilityAssigner final : public ITaskAssigner
    {
    public:
        auto assign(const PlanStep &step, AgentRegistry &reg) -> Core::Result<std::string> override
        {
            IAgentRuntime *a = reg.pick(step.capability);
            if (!a)
                return Core::Result<std::string>::Error(
                    Core::Error(Core::ErrorCode::NotFound,
                                "no agent for capability '" + step.capability + "'"));
            return Core::Result<std::string>::Ok(a->spec().id);
        }
    };

} // namespace AE::Engine
