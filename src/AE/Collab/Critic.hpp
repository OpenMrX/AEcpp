#pragma once

/// @file Collab/Critic.hpp
/// @brief 协作层提交评审接口（Supervisor / Debate 复用）（Docs/01 §5.2）。
///
/// @details 与单 Agent 的 `IReflector`（需 `ModeContext`）解耦：协作层评审以 `CollabContext`
///          为边界，复用 `Critique` 结构（`Mode/IReflector.hpp`）。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <string_view>

#include "Core/Result.hpp"
#include "Mode/IReflector.hpp" // Critique
#include "Collab/ICollaboration.hpp"

namespace AE::Engine
{

    class ISubmissionCritic
    {
    public:
        virtual ~ISubmissionCritic() = default;
        virtual auto review(std::string_view output, std::string_view criteria, CollabContext &ctx)
            -> Core::Result<Critique> = 0;
    };

    // 默认：全部通过（无评审）。
    class PassThroughCritic final : public ISubmissionCritic
    {
    public:
        auto review(std::string_view, std::string_view, CollabContext &) -> Core::Result<Critique> override
        {
            Critique c;
            c.passed = true;
            c.score = 1.0;
            return Core::Result<Critique>::Ok(std::move(c));
        }
    };

} // namespace AE::Engine
