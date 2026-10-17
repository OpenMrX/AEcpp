#pragma once

/// @file Mapping/Router.hpp
/// @brief Handoff / 路由原语（OpenAI Agents SDK 隐喻，Docs/01 §6.2、§6.5）。
///
/// @details `Handoff` 描述「从→到 + 触发条件」；`RouterAgent::route` 依据状态选择下一跳。
///
/// @note 命名空间为扁平 `AE::Engine`（评审结论 #1）。

#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "nlohmann-json.hpp"

namespace AE::Engine
{

    struct Handoff
    {
        std::string from;
        std::string to;
        std::function<bool(const nlohmann::json &)> when; // 空 = 无条件
    };

    class RouterAgent
    {
    public:
        // 选择首条 `when(state)` 成立的 `to`；无匹配返回空串。
        [[nodiscard]] auto route(const nlohmann::json &state,
                                 std::span<const Handoff> handoffs) const -> std::string
        {
            for (const auto &h : handoffs)
                if (!h.when || h.when(state))
                    return h.to;
            return {};
        }
    };

} // namespace AE::Engine
