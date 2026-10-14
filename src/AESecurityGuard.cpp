// =============================================================================
//  AE::Engine — AESecurityGuard.cpp
//  安全组件实现：路径验证、Shell 注入防护、持久化权限。
//  从 React+Security.cpp 迁移，命名空间改为 AE::Engine。
// =============================================================================

#include "AESecurityGuard.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "nlohmann-json.hpp"

#include "Core/Logger.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstring>
#include <limits.h>
#include <stdlib.h>
#endif

namespace AE::Engine
{

    // ================================================================
    //  路径 canonicalize（跨平台）
    // ================================================================

    namespace
    {
#ifdef _WIN32
        [[nodiscard]] auto canonicalize_path(std::string_view path) -> std::string
        {
            // 父目录规范化（realpath 在 Windows 上要求路径存在）
            std::wstring wpath(path.begin(), path.end());
            wchar_t buf[MAX_PATH] = {0};
            if (GetFullPathNameW(wpath.c_str(), MAX_PATH, buf, nullptr) == 0)
                return {};

            std::string result;
            auto len = std::wcslen(buf);
            result.reserve(len);
            for (std::size_t i = 0; i < len; ++i)
                result += static_cast<char>(buf[i]); // ASCII subset

            // normalize backslash to forward slash
            std::ranges::replace(result, '\\', '/');
            return result;
        }
#else
        [[nodiscard]] auto canonicalize_path(std::string_view path) -> std::string
        {
            // POSIX: 先规范化父目录
            std::filesystem::path p{std::string{path}};
            auto parent = p.parent_path();
            if (!std::filesystem::exists(parent))
                return {};

            char resolved[PATH_MAX] = {0};
            if (::realpath(parent.c_str(), resolved) == nullptr)
                return {};

            return std::string{resolved} + "/" + p.filename().string();
        }
#endif
    } // anonymous namespace

    auto validate_path(std::string_view base_path, std::string_view requested_path) -> bool
    {
        if (base_path.empty() || requested_path.empty())
            return false;

        auto base = canonicalize_path(base_path);
        auto req = canonicalize_path(requested_path);

        if (base.empty() || req.empty())
            return false;

        // 确保 base 以 '/' 结尾
        if (!base.ends_with('/'))
            base += '/';

        // 检查 req 是否在 base 之下
        return req.starts_with(base) || req == base.substr(0, base.size() - 1);
    }

    auto is_safe_shell_arg(std::string_view arg) -> bool
    {
        if (arg.empty())
            return false;

        constexpr std::string_view k_unsafe_chars = "|&;<>$`\\\"'(){}[]!#~*?";
        for (char c : arg)
        {
            if (k_unsafe_chars.find(c) != std::string_view::npos)
                return false;
        }
        return true;
    }

    // ================================================================
    //  AllowedToolsStore
    // ================================================================

    namespace
    {
        [[nodiscard]] auto default_store_dir() -> std::string
        {
#ifdef _WIN32
            const char *home = std::getenv("USERPROFILE");
#else
            const char *home = std::getenv("HOME");
#endif
            std::string base = home ? std::string{home} : ".";
            return base + "/.ae/security";
        }

        [[nodiscard]] auto store_file_path(const std::string &dir) -> std::string
        {
            return dir + "/allowed_tools.json";
        }
    } // anonymous namespace

    AllowedToolsStore::AllowedToolsStore()
        : AllowedToolsStore(default_store_dir())
    {
    }

    AllowedToolsStore::AllowedToolsStore(std::string_view storage_dir)
        : storage_path_(store_file_path(std::string{storage_dir}))
    {
        load();
    }

    auto AllowedToolsStore::is_always_allowed(std::string_view tool_name) const -> bool
    {
        std::shared_lock lock(mutex_);
        return allowed_.contains(std::string{tool_name});
    }

    void AllowedToolsStore::add_always_allowed(std::string_view tool_name)
    {
        {
            std::scoped_lock lock(mutex_);
            allowed_.emplace(std::string{tool_name});
        }
        save();
    }

    void AllowedToolsStore::remove_always_allowed(std::string_view tool_name)
    {
        {
            std::scoped_lock lock(mutex_);
            allowed_.erase(std::string{tool_name});
        }
        save();
    }

    auto AllowedToolsStore::all_allowed() const -> std::vector<std::string>
    {
        std::shared_lock lock(mutex_);
        return {allowed_.begin(), allowed_.end()};
    }

    void AllowedToolsStore::clear_all()
    {
        {
            std::scoped_lock lock(mutex_);
            allowed_.clear();
        }
        save();
    }

    void AllowedToolsStore::load()
    {
        try
        {
            std::ifstream f{storage_path_};
            if (!f.is_open())
                return; // 首次使用，无文件

            auto j = nlohmann::json::parse(f);
            if (!j.contains("allowed_tools") || !j["allowed_tools"].is_array())
                return;

            std::scoped_lock lock(mutex_);
            allowed_.clear();
            for (const auto &name : j["allowed_tools"])
            {
                if (name.is_string())
                    allowed_.insert(name.get<std::string>());
            }
        }
        catch (...)
        {
            Core::Logger::getInstance().warningFmt(
                "AllowedToolsStore: failed to load from {}, starting empty", storage_path_);
        }
    }

    void AllowedToolsStore::save() const
    {
        try
        {
            // 构建 JSON
            auto j = nlohmann::json::object();
            auto arr = nlohmann::json::array();
            {
                std::shared_lock lock(mutex_);
                for (const auto &name : allowed_)
                    arr.push_back(name);
            }
            j["allowed_tools"] = std::move(arr);

            // 原子写入：temp file + rename
            std::filesystem::path dir = std::filesystem::path{storage_path_}.parent_path();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);

            auto temp_path = storage_path_ + ".tmp";
            {
                std::ofstream f{temp_path, std::ios::trunc};
                if (!f.is_open())
                {
                    Core::Logger::getInstance().warning(
                        "AllowedToolsStore: cannot open temp file for writing");
                    return;
                }
                f << j.dump(2);
                if (!f.good())
                {
                    Core::Logger::getInstance().warning(
                        "AllowedToolsStore: write to temp file failed");
                    return;
                }
            }

            std::filesystem::rename(temp_path, storage_path_, ec);
            if (ec)
            {
                Core::Logger::getInstance().warningFmt(
                    "AllowedToolsStore: rename failed: {}", ec.message());
            }
        }
        catch (const std::exception &e)
        {
            Core::Logger::getInstance().warningFmt(
                "AllowedToolsStore: save failed: {}", e.what());
        }
    }

} // namespace AE::Engine
