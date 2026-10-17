#pragma once

/// @file Ports/ToolPort.hpp
/// @brief 工具 Port：动态工具源、聚合注册表、工具失败分类（Docs/01 §7.1 + §4.2 R4）。
///
/// @details
/// 设计约束（关键）：
/// - 只依赖 `Core::Result` / `nlohmann::json` / 标准库；不 include 任何宿主实现。
/// - 既有 `IToolProvider`（AEType.hpp）**保持不变**；本层新增「多源聚合」与「失败分类」，
///   对上层仍以 `IToolProvider` 暴露——抽象只出现在「每工具调用一次」的边界。
/// - `ToolResult.content` 为**不可信**文本，回灌 LLM 前须经 `ToolOutputSanitizer`。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Error.hpp"
#include "AEType.hpp" // IToolProvider

namespace AE::Engine
{

    // ---------------------------------------------------- 工具声明（静态元数据）
    // 与 `IToolProvider::get_tools_schema()` 的 OpenAI 兼容条目一一对应。
    struct ToolSpec
    {
        std::string name;
        std::string description;
        nlohmann::json parameters = nlohmann::json::object(); // JSON Schema（object）
        std::string ns;                                       // 源命名空间（可空）

        [[nodiscard]] auto qualified_name() const -> std::string
        {
            return ns.empty() ? name : (ns + '.' + name);
        }

        // 归一化为 OpenAI function 条目。
        [[nodiscard]] auto to_openai_function() const -> nlohmann::json
        {
            return nlohmann::json::object(
                {{"type", "function"},
                 {"function", nlohmann::json::object({{"name", name},
                                                      {"description", description},
                                                      {"parameters", parameters}})}});
        }
    };

    // ---------------------------------------------------- 工具结果（跨源统一）
    struct ToolResult
    {
        bool ok = false;
        std::string content;                 // 成功载荷（不可信文本，回灌前需净化）
        std::optional<Core::Error> error;    // 失败原因（ok==false）

        static auto success(std::string c) -> ToolResult
        {
            return ToolResult{true, std::move(c), std::nullopt};
        }
        static auto failure(Core::Error e) -> ToolResult
        {
            return ToolResult{false, {}, std::move(e)};
        }

        [[nodiscard]] auto to_json() const -> nlohmann::json
        {
            nlohmann::json j = nlohmann::json::object();
            j["ok"] = ok;
            if (ok)
                j["content"] = content;
            else
                j["error"] = error ? error->message() : std::string{};
            return j;
        }
    };

    // ---------------------------------------------------- 动态工具源（MCP / 插件）
    class IToolSource
    {
    public:
        virtual ~IToolSource() = default;
        [[nodiscard]] virtual auto list_tools() const -> std::vector<ToolSpec> = 0;
        virtual auto call_tool(std::string_view name, const nlohmann::json &args)
            -> Core::Result<ToolResult> = 0;
    };

    // ---------------------------------------------------- 工具注册表（多源聚合）
    // 对上层仍是 IToolProvider：schema 汇集所有源；execute 按 `ns.name` 路由到源。
    class IToolRegistry : public IToolProvider
    {
    public:
        // 注册一个工具源（ns 为空表示匿名根源）。不持有源所有权（生命周期由调用方保证）。
        virtual auto add_source(std::string ns, IToolSource *src) -> void = 0;
        // 解析限定名（`ns.name` 或裸名）到具体源。
        virtual auto resolve(std::string_view id) -> Core::Result<IToolSource *> = 0;
    };

    // ---------------------------------------------------- 工具失败分类（R4）
    enum class ToolFailureClass : std::uint8_t
    {
        Ok,          // 成功
        Retryable,   // 可重试（超时 / 瞬时网络 / 源不可用）
        Recoverable, // 可降级（业务性失败，回灌错误文本让模型改道）
        Fatal        // 致命（权限 / 非法参数 / 内部错误），应中止
    };

    // 失败分类策略：每次「工具结果」调用一次（边界，非热路径）。
    // 默认启发式实现见 HeuristicToolFailurePolicy。
    class ToolFailurePolicy
    {
    public:
        virtual ~ToolFailurePolicy() = default;
        [[nodiscard]] virtual auto classify(std::string_view name,
                                            const Core::Result<nlohmann::json> &result) const
            -> ToolFailureClass = 0;
    };

    // 启发式默认实现：按错误码归类（未知 → Recoverable）。
    class HeuristicToolFailurePolicy final : public ToolFailurePolicy
    {
    public:
        [[nodiscard]] auto classify(std::string_view /*name*/,
                                    const Core::Result<nlohmann::json> &result) const
            -> ToolFailureClass override
        {
            if (result.isSuccess())
                return ToolFailureClass::Ok;
            using enum Core::ErrorCode;
            switch (result.error().code())
            {
            case Timeout:
            case NetworkError:
            case PortTimeout:
            case PortUnavailable:
            case Busy:
                return ToolFailureClass::Retryable;
            case PermissionDenied:
            case PortDenied:
            case InvalidArgument:
            case PortInvalidArg:
            case InternalError:
            case PortInternal:
                return ToolFailureClass::Fatal;
            default:
                return ToolFailureClass::Recoverable;
            }
        }
    };

} // namespace AE::Engine
