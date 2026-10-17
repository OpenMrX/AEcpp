#pragma once

/// @file Ports/ModelPort.hpp
/// @brief 模型 Port：多模型补全 / 强弱模型分派（Docs/01 §7.5）。
///
/// @details
/// 设计约束（关键）：
/// - 只依赖 `Core::Result` / `nlohmann::json` / 标准库。
/// - 既有 `LLMAdapter`（LLMAdapter.hpp）可作为 `IModelProvider` 适配。
/// - `pick` 以 `role` 字符串（而非枚举）表达分派意图，避免与协作层角色定义过早耦合；
///   决策只发生在「每任务一次」的边界。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <string>
#include <string_view>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"

namespace AE::Engine
{

    // ---------------------------------------------------- 模型补全
    class IModelProvider
    {
    public:
        virtual ~IModelProvider() = default;
        // model 为空表示「由实现自决」；req 为后端请求体（OpenAI 兼容或归一化）。
        virtual auto complete(std::string_view model, const nlohmann::json &req)
            -> Core::Result<nlohmann::json> = 0;
    };

    // ---------------------------------------------------- 模型路由（强弱 / 成本）
    class IModelRouter
    {
    public:
        virtual ~IModelRouter() = default;
        // 依据角色与任务返回 model id（空表示沿用默认）。
        virtual auto pick(std::string_view role, const nlohmann::json &task) -> std::string = 0;
    };

    // 默认路由：恒定返回空（沿用默认模型）。
    class DefaultModelRouter final : public IModelRouter
    {
    public:
        auto pick(std::string_view /*role*/, const nlohmann::json & /*task*/) -> std::string override
        {
            return {};
        }
    };

} // namespace AE::Engine
