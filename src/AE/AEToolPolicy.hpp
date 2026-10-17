#pragma once

/// @file AEToolPolicy.hpp
/// @brief 工具安全策略引擎：Allow/Confirm/Deny 三级策略 + 参数校验 + 白名单存储。
///
/// @details
/// 从 React+ToolRegistry 迁移，适配 AE::IToolProvider 接口。
///
/// 设计：
/// - AEToolPolicy 是 IToolProvider 的装饰器（Decorator）：包装底层 IToolProvider，
/// 在执行前做策略检查（Allow/Confirm/Deny）和参数校验。
/// - AllowedToolsStore 已在 AESecurityGuard 中（B1-2），此处直接引用。
///
/// nlohmann::json、Core::Result。
///
/// @note 所属命名空间：AE::Engine
/// @note 依赖：AEType.hpp（IToolProvider）、AESecurityGuard.hpp（AllowedToolsStore）、

#include <flat_map>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "AEType.hpp"
#include "AESecurityGuard.hpp"

namespace AE::Engine
{

    // ================================================================
    //  枚举
    // ================================================================

    enum class ToolPolicy : std::uint8_t
    {
        Allow,
        Confirm,
        Deny
    };

    enum class ToolConfirmResult : std::uint8_t
    {
        AllowOnce,
        AlwaysAllow,
        Deny
    };

    enum class ToolParamType : std::uint8_t
    {
        String,
        Integer,
        Number,
        Boolean,
        Array,
        Object
    };

    // ================================================================
    //  ToolParameter — 工具参数声明
    // ================================================================

    struct ToolParameter
    {
        std::string name;
        ToolParamType type{ToolParamType::String};
        bool required{true};
        std::string description;
    };

    // ================================================================
    //  辅助函数
    // ================================================================

    [[nodiscard]] inline auto param_type_to_string(ToolParamType t) -> std::string_view
    {
        using enum ToolParamType;
        switch (t)
        {
        case String:  return "string";
        case Integer: return "integer";
        case Number:  return "number";
        case Boolean: return "boolean";
        case Array:   return "array";
        case Object:  return "object";
        }
        return "string";
    }

    [[nodiscard]] inline auto string_to_param_type(std::string_view s) -> ToolParamType
    {
        if (s == "integer") return ToolParamType::Integer;
        if (s == "number")  return ToolParamType::Number;
        if (s == "boolean") return ToolParamType::Boolean;
        if (s == "array")   return ToolParamType::Array;
        if (s == "object")  return ToolParamType::Object;
        return ToolParamType::String;
    }

    // ================================================================
    //  ToolRegistryEntry — 注册表条目
    // ================================================================

    struct ToolRegistryEntry
    {
        std::string name;
        std::string description;
        std::vector<ToolParameter> parameters;
        ToolPolicy policy{ToolPolicy::Allow};
        bool enabled{true};
    };

    // ================================================================
    //  ToolPolicyRegistry — 工具策略注册表（线程安全）
    //
    //  管理工具的元数据（名称、描述、参数、策略、启用状态）。
    //  工具的实际执行委托给底层 IToolProvider。
    // ================================================================

    class ToolPolicyRegistry
    {
    public:
        ToolPolicyRegistry() = default;
        ~ToolPolicyRegistry() = default;

        ToolPolicyRegistry(const ToolPolicyRegistry&) = delete;
        ToolPolicyRegistry& operator=(const ToolPolicyRegistry&) = delete;

        // ---- 注册 ----
        auto registerTool(ToolRegistryEntry entry) -> Core::Result<void>;
        auto registerTool(std::string_view name, std::string_view description,
            std::vector<ToolParameter> params = {},
            ToolPolicy policy = ToolPolicy::Allow) -> Core::Result<void>;

        // ---- 查找 ----
        [[nodiscard]] auto find(std::string_view name) const -> const ToolRegistryEntry*;
        [[nodiscard]] auto resolveName(std::string_view name) const -> std::string;
        [[nodiscard]] auto has(std::string_view name) const -> bool;
        [[nodiscard]] auto size() const -> std::size_t;

        // ---- Schema ----
        [[nodiscard]] auto getToolsSchema() const -> nlohmann::json;

        // ---- 启用/禁用 ----
        [[nodiscard]] auto setEnabled(std::string_view name, bool enabled) -> bool;
        [[nodiscard]] auto isEnabled(std::string_view name) const -> bool;

        // ---- 安全配置 ----
        void setConfirmCallback(std::function<ToolConfirmResult(
            std::string_view, const nlohmann::json&)> cb);
        void setDefaultPolicy(ToolPolicy policy) noexcept;
        [[nodiscard]] auto defaultPolicy() const noexcept -> ToolPolicy;
        void setAllowedStore(std::shared_ptr<AllowedToolsStore> store);

        // ---- 参数校验 ----
        [[nodiscard]] auto validateArgs(std::string_view toolName,
            const nlohmann::json& args) const -> Core::Result<nlohmann::json>;

    private:
        static auto toLower(std::string_view s) -> std::string;

        std::flat_map<std::string, ToolRegistryEntry, std::less<>> entries_;
        mutable std::shared_mutex mutex_;
        ToolPolicy default_policy_{ToolPolicy::Allow};
        std::function<ToolConfirmResult(std::string_view, const nlohmann::json&)> confirm_cb_;
        std::shared_ptr<AllowedToolsStore> allowed_store_;
    };

    // ================================================================
    //  PolicyEnforcingToolProvider — IToolProvider 安全装饰器
    //
    //  包装底层 IToolProvider，在执行前检查 ToolPolicyRegistry 中的策略。
    //  流程：查找 → 启用检查 → DENY 拒绝 → 参数校验 → CONFIRM 确认 → 委托执行。
    // ================================================================

    class PolicyEnforcingToolProvider : public IToolProvider
    {
    public:
        PolicyEnforcingToolProvider(IToolProvider& delegate,
            ToolPolicyRegistry& registry);

        auto execute(std::string_view name, const nlohmann::json &args, int max_retries = 3)
            -> Core::Result<nlohmann::json> override;
        auto list_tools() const -> std::vector<std::string> override;
        auto get_tools_schema() const -> nlohmann::json override;

        // 暴露底层 delegate 和 registry，供高级用例使用
        [[nodiscard]] auto delegate() noexcept -> IToolProvider& { return delegate_; }
        [[nodiscard]] auto registry() noexcept -> ToolPolicyRegistry& { return registry_; }

    private:
        IToolProvider& delegate_;
        ToolPolicyRegistry& registry_;
    };

} // namespace AE::Engine
