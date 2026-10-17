#pragma once

/// @file Collab/AgentSpec.hpp
/// @brief 协作层角色/能力/模型/模式配置（Docs/01 §5.0.1）。
///
/// @details 纯数据；只依赖 `Mode/ModeKind.hpp` 与标准库。`tool_ids` 经 `IToolRegistry`
///          解析（§7.1）；`memory_scope` 为记忆隔离命名空间（§7.2）。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <string>
#include <string_view>
#include <vector>

#include "Mode/ModeKind.hpp"

namespace AE::Engine
{

    struct AgentSpec
    {
        std::string id;
        std::string name;
        std::string system_prompt;
        std::string model;                 // 覆盖默认模型（§5.2 强弱分派）
        ModeKind mode{ModeKind::React};
        std::vector<std::string> tool_ids; // 经 IToolRegistry 解析（§7.1）
        std::string memory_scope;          // 记忆隔离命名空间（§7.2）
        std::vector<std::string> capabilities;

        [[nodiscard]] auto has_capability(std::string_view cap) const -> bool
        {
            for (const auto &c : capabilities)
                if (c == cap)
                    return true;
            return false;
        }
    };

} // namespace AE::Engine
