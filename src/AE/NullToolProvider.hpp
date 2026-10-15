#pragma once

/// @file NullToolProvider.hpp
/// @brief 无工具的 IToolProvider 实现：供 Butler 在 DCEE 未连接（AE_ENABLE_DCEE=OFF）
///
/// @details
/// 场景下跑通 TUI → Butler → AgentEngine 链路。
///
/// 语义：工具面为空，get_tools_schema() 返回空数组 → AgentEngine 主循环
/// has_tool==false 分支直接 co_yield Final 收尾（单轮 LLM，无工具循环）。
/// 这正好满足「先跑通链路、工具能力后续接入」的阶段性目标。
///
/// @note 所属命名空间：AE::Engine
/// @note 设计约束：纯本地、零外部依赖（不依赖 DCEE），保证 AE::Engine 独立编译。

#include <string_view>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Error.hpp"
#include "IAE.hpp"

namespace AE::Engine
{

    // 无工具 provider：工具清单/ schema 均为空；execute 不可达（LLM 不会请求工具）。
    class NullToolProvider final : public IToolProvider
    {
    public:
        ~NullToolProvider() override = default;

        auto execute(std::string_view name,
                     const nlohmann::json& /*args*/,
                     int /*max_retries*/ = 3) -> Core::Result<nlohmann::json> override
        {
            // 工具面为空，正常不应被调用；若被调用说明 LLM 误生成了工具调用，
            // 直接返回结构化错误，交由 AgentEngine 的 OnError hook 处理或继续循环。
            return Core::Result<nlohmann::json>::Error(
                Core::Error{"NullToolProvider::execute: no tools registered "
                            "(tool '" + std::string{name} + "' unavailable)"});
        }

        [[nodiscard]] auto list_tools() const -> std::vector<std::string> override
        {
            return {};
        }

        [[nodiscard]] auto get_tools_schema() const -> nlohmann::json override
        {
            return nlohmann::json::array();
        }
    };

} // namespace AE::Engine
