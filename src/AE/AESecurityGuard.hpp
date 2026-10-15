#pragma once

/// @file AESecurityGuard.hpp
/// @brief 安全组件：路径验证、Shell 注入防护、持久化工具权限存储。
///
/// @details
/// 从 React+Security 迁移，命名空间改为 AE::Engine。
///
/// @note 所属命名空间：AE::Engine
/// @note 依赖：Core::Result、nlohmann::json（AllowedToolsStore 持久化）、Core::Logger。

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace AE::Engine
{

    // ================================================================
    //  路径安全
    // ================================================================

    // 验证 requested_path 是否在 base_path 目录树内（防目录遍历攻击）。
    [[nodiscard]] auto validate_path(std::string_view base_path,
        std::string_view requested_path) -> bool;

    // 检查 Shell 参数是否包含危险字符（管道、重定向、命令注入等）。
    [[nodiscard]] auto is_safe_shell_arg(std::string_view arg) -> bool;

    // ================================================================
    //  AllowedToolsStore — 用户确认过的工具白名单（跨实例持久化）
    // ================================================================

    class AllowedToolsStore
    {
    public:
        AllowedToolsStore();
        explicit AllowedToolsStore(std::string_view storage_dir);
        ~AllowedToolsStore() = default;

        // 不可拷贝（持有 mutex）
        AllowedToolsStore(const AllowedToolsStore&) = delete;
        AllowedToolsStore& operator=(const AllowedToolsStore&) = delete;

        [[nodiscard]] auto is_always_allowed(std::string_view tool_name) const -> bool;
        void add_always_allowed(std::string_view tool_name);
        void remove_always_allowed(std::string_view tool_name);
        [[nodiscard]] auto all_allowed() const -> std::vector<std::string>;
        void clear_all();

    private:
        void load();
        void save() const;

        std::string storage_path_;
        mutable std::shared_mutex mutex_;
        std::unordered_set<std::string> allowed_;
    };

} // namespace AE::Engine
