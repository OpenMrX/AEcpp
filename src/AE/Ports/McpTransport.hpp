#pragma once

/// @file Ports/McpTransport.hpp
/// @brief MCP 传输抽象 + MCP→IToolSource 适配（Docs/01 §7.1、§四.2）。
///
/// @details
/// 设计约束（关键）：
/// - 只依赖 `Core::Result` / `nlohmann::json` / 标准库。
/// - 抽象只描述 MCP 的「工具面」三操作（list/call/notify）+ 可选握手；具体传输
///   （stdio / Streamable-HTTP）由实现提供（见 McpHttpClient.hpp）。
/// - `McpToolSource` 把任意 `IMcpTransport` 适配为 `IToolSource`，从而并入 `IToolRegistry`。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <string>
#include <string_view>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Ports/ToolPort.hpp"

namespace AE::Engine
{

    // ---------------------------------------------------- MCP 传输
    class IMcpTransport
    {
    public:
        virtual ~IMcpTransport() = default;

        // 握手（initialize / notifications/initialized）；默认无需握手 → 直接成功。
        virtual auto initialize() -> Core::Result<void> { return Core::Result<void>::Ok(); }

        virtual auto list_tools() -> Core::Result<std::vector<ToolSpec>> = 0;
        virtual auto call_tool(std::string_view name, const nlohmann::json &args)
            -> Core::Result<ToolResult> = 0;
        // Server 方向通知（可选能力）。
        virtual auto notify(std::string_view method, const nlohmann::json &params)
            -> Core::Result<void> = 0;
    };

    // ---------------------------------------------------- MCP → IToolSource 适配
    // 不持有 transport 所有权（生命周期由调用方保证）。
    class McpToolSource final : public IToolSource
    {
    public:
        McpToolSource(IMcpTransport &transport, std::string ns = {})
            : m_transport(transport), m_ns(std::move(ns)) {}

        [[nodiscard]] auto list_tools() const -> std::vector<ToolSpec> override
        {
            auto r = m_transport.list_tools();
            if (r.isError())
                return {};
            auto out = std::move(r.value());
            for (auto &t : out)
                if (t.ns.empty())
                    t.ns = m_ns;
            return out;
        }

        auto call_tool(std::string_view name, const nlohmann::json &args)
            -> Core::Result<ToolResult> override
        {
            return m_transport.call_tool(name, args);
        }

    private:
        IMcpTransport &m_transport;
        std::string m_ns;
    };

} // namespace AE::Engine
