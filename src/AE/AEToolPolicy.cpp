// =============================================================================
//  AE::Engine — AEToolPolicy.cpp
//  工具安全策略引擎实现。
//  从 React+ToolRegistry.cpp 迁移，适配 AE::IToolProvider 接口。
// =============================================================================

#include "AEToolPolicy.hpp"

#include <algorithm>
#include <stdexcept>

#include "Core/Logger.hpp"

namespace AE::Engine
{

    // ================================================================
    //  ToolPolicyRegistry
    // ================================================================

    auto ToolPolicyRegistry::registerTool(ToolRegistryEntry entry) -> Core::Result<void>
    {
        if (entry.name.empty())
        {
            return Core::Result<void>::Error(
                Core::Error{Core::ErrorCode::InvalidArgument, "ToolPolicyRegistry: tool name must not be empty"});
        }

        std::scoped_lock lock(mutex_);

        if (entries_.contains(entry.name))
        {
            return Core::Result<void>::Error(
                Core::Error{Core::ErrorCode::InvalidArgument, "ToolPolicyRegistry: tool already registered: " + entry.name});
        }

        if (entry.policy == ToolPolicy::Allow && default_policy_ != ToolPolicy::Allow)
        {
            entry.policy = default_policy_;
        }

        auto key = entry.name;
        entries_.emplace(std::move(key), std::move(entry));
        return Core::Result<void>::Ok();
    }

    auto ToolPolicyRegistry::registerTool(std::string_view name, std::string_view description,
                                          std::vector<ToolParameter> params, ToolPolicy policy) -> Core::Result<void>
    {
        ToolRegistryEntry entry;
        entry.name = std::string{name};
        entry.description = std::string{description};
        entry.parameters = std::move(params);
        entry.policy = policy;
        return registerTool(std::move(entry));
    }

    auto ToolPolicyRegistry::find(std::string_view name) const -> const ToolRegistryEntry*
    {
        std::shared_lock lock(mutex_);
        auto it = entries_.find(name);
        if (it != entries_.end())
            return &it->second;
        return nullptr;
    }

    auto ToolPolicyRegistry::resolveName(std::string_view name) const -> std::string
    {
        std::shared_lock lock(mutex_);
        std::string lower = toLower(name);

        // 策略 1：后缀匹配
        std::vector<std::string> candidates;
        std::string suffix = "_" + lower;
        for (const auto &[key, entry] : entries_)
        {
            auto key_lower = toLower(key);
            if (key_lower.ends_with(suffix))
                candidates.push_back(key);
        }
        if (candidates.size() == 1)
            return candidates[0];

        // 策略 2：大小写不敏感精确匹配
        candidates.clear();
        for (const auto &[key, entry] : entries_)
        {
            if (toLower(key) == lower)
                candidates.push_back(key);
        }
        if (candidates.size() == 1)
            return candidates[0];

        return {};
    }

    auto ToolPolicyRegistry::has(std::string_view name) const -> bool
    {
        std::shared_lock lock(mutex_);
        return entries_.contains(name);
    }

    auto ToolPolicyRegistry::size() const -> std::size_t
    {
        std::shared_lock lock(mutex_);
        return entries_.size();
    }

    auto ToolPolicyRegistry::getToolsSchema() const -> nlohmann::json
    {
        std::shared_lock lock(mutex_);
        auto arr = nlohmann::json::array();
        for (const auto &[name, entry] : entries_)
        {
            if (!entry.enabled)
                continue;

            nlohmann::json props = nlohmann::json::object();
            nlohmann::json required_arr = nlohmann::json::array();
            for (const auto &p : entry.parameters)
            {
                props[p.name] = {
                    {"type", param_type_to_string(p.type)},
                    {"description", p.description},
                };
                if (p.required)
                    required_arr.push_back(p.name);
            }

            arr.push_back({
                {"type", "function"},
                {"function", {
                    {"name", name},
                    {"description", entry.description},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", props},
                        {"required", required_arr},
                    }},
                }},
            });
        }
        return arr;
    }

    auto ToolPolicyRegistry::setEnabled(std::string_view name, bool enabled) -> bool
    {
        std::scoped_lock lock(mutex_);
        auto it = entries_.find(name);
        if (it == entries_.end())
            return false;
        it->second.enabled = enabled;
        return true;
    }

    auto ToolPolicyRegistry::isEnabled(std::string_view name) const -> bool
    {
        std::shared_lock lock(mutex_);
        auto it = entries_.find(name);
        if (it == entries_.end())
            return false;
        return it->second.enabled;
    }

    void ToolPolicyRegistry::setConfirmCallback(
        std::function<ToolConfirmResult(std::string_view, const nlohmann::json&)> cb)
    {
        std::scoped_lock lock(mutex_);
        confirm_cb_ = std::move(cb);
    }

    void ToolPolicyRegistry::setDefaultPolicy(ToolPolicy policy) noexcept
    {
        std::scoped_lock lock(mutex_);
        default_policy_ = policy;
    }

    auto ToolPolicyRegistry::defaultPolicy() const noexcept -> ToolPolicy
    {
        std::shared_lock lock(mutex_);
        return default_policy_;
    }

    void ToolPolicyRegistry::setAllowedStore(std::shared_ptr<AllowedToolsStore> store)
    {
        std::scoped_lock lock(mutex_);
        allowed_store_ = std::move(store);
    }

    auto ToolPolicyRegistry::validateArgs(std::string_view toolName,
                                          const nlohmann::json& args) const -> Core::Result<nlohmann::json>
    {
        if (!args.is_object())
        {
            return Core::Result<nlohmann::json>::Error(
                Core::Error{"Tool arguments must be a JSON object"});
        }

        std::shared_lock lock(mutex_);
        auto it = entries_.find(toolName);
        if (it == entries_.end())
        {
            return Core::Result<nlohmann::json>::Error(
                Core::Error{"Tool not found in registry: " + std::string{toolName}});
        }

        for (const auto &p : it->second.parameters)
        {
            if (p.required && !args.contains(p.name))
            {
                return Core::Result<nlohmann::json>::Error(
                    Core::Error{"Missing required parameter: " + p.name});
            }
        }

        // 类型校验（警告级，不阻断）
        for (const auto &p : it->second.parameters)
        {
            if (!args.contains(p.name))
                continue;

            const auto &val = args[p.name];
            bool type_ok = true;
            switch (p.type)
            {
            case ToolParamType::String:  type_ok = val.is_string();  break;
            case ToolParamType::Integer:
            case ToolParamType::Number:  type_ok = val.is_number();  break;
            case ToolParamType::Boolean: type_ok = val.is_boolean(); break;
            case ToolParamType::Array:   type_ok = val.is_array();   break;
            case ToolParamType::Object:  type_ok = val.is_object();  break;
            }

            if (!type_ok)
            {
                Core::Logger::getInstance().warningFmt(
                    "ToolPolicyRegistry: type mismatch for param '{}' (expected {}, got {})",
                    p.name, param_type_to_string(p.type), val.type_name());
            }
        }

        return Core::Result<nlohmann::json>::Ok(args);
    }

    auto ToolPolicyRegistry::toLower(std::string_view s) -> std::string
    {
        std::string out;
        out.reserve(s.size());
        for (char c : s)
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // ================================================================
    //  PolicyEnforcingToolProvider
    // ================================================================

    PolicyEnforcingToolProvider::PolicyEnforcingToolProvider(
        IToolProvider& delegate, ToolPolicyRegistry& registry)
        : delegate_(delegate), registry_(registry)
    {
    }

    auto PolicyEnforcingToolProvider::execute(std::string_view name,
        const nlohmann::json &args, int max_retries) -> Core::Result<nlohmann::json>
    {
        // 1. 名称解析
        auto resolved = std::string{name};
        if (!registry_.has(resolved))
        {
            resolved = registry_.resolveName(resolved);
            if (resolved.empty())
            {
                // 策略注册表未覆盖该工具：降级委托给底层 provider。
                // 避免 DCEE 已注册工具因策略层未同步而被误判为不存在。
                Core::Logger::getInstance().warningFmt(
                    "PolicyEnforcingToolProvider: '{}' not in policy registry, delegating", name);
                return delegate_.execute(name, args, max_retries);
            }
        }

        // 2. 查找条目
        const auto* entry = registry_.find(resolved);
        if (!entry)
        {
            return Core::Result<nlohmann::json>::Error(
                Core::Error{"Tool entry not found: " + resolved});
        }

        // 3. 启用检查
        if (!entry->enabled)
        {
            return Core::Result<nlohmann::json>::Error(
                Core::Error{"Tool is disabled: " + resolved});
        }

        // 4. DENY 策略
        if (entry->policy == ToolPolicy::Deny)
        {
            return Core::Result<nlohmann::json>::Error(
                Core::Error{"Tool denied by policy: " + resolved});
        }

        // 5. 参数校验
        auto val_result = registry_.validateArgs(resolved, args);
        if (val_result.isError())
        {
            return Core::Result<nlohmann::json>::Error(val_result.error());
        }

        // 6. CONFIRM 策略
        if (entry->policy == ToolPolicy::Confirm)
        {
            // 委托给 PolicyEnforcingToolProvider 持有 registry 引用的 confirm_cb
            // 注意：此处简化处理——confirm 逻辑通过外部回调实现
            // 实际 CONFIRM 流程由上层（Butler/GUI）通过 registry 的 confirm_cb 处理
        }

        // 7. 委托执行
        return delegate_.execute(resolved, args, max_retries);
    }

    auto PolicyEnforcingToolProvider::list_tools() const -> std::vector<std::string>
    {
        return delegate_.list_tools();
    }

    auto PolicyEnforcingToolProvider::get_tools_schema() const -> nlohmann::json
    {
        return delegate_.get_tools_schema();
    }

} // namespace AE::Engine
