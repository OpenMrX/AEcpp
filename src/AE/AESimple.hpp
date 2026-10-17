#pragma once

/// @file AESimple.hpp
/// @brief 最小本地实现：SimpleContextSource / SimpleToolProvider / SimpleStateStore。
///
/// @details
/// 让 AE::Engine 可独立编译自跑（无宿主工具 / 上下文 / 状态设施）。生产接入时用
/// 宿主适配器替换即可，AgentEngine 代码无需改动。
///
/// 结构性调整：本文件实现原位于已移除的 IAE.hpp；接口（IContextSource /
/// IToolProvider / IStateStore）现位于 AEType.hpp。
///
/// @note 所属命名空间：AE::Engine

#include <algorithm>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Error.hpp"
#include "AEType.hpp"

namespace AE::Engine
{

    // ---------------------------------------------------- SimpleContextSource
    class SimpleContextSource : public IContextSource
    {
    public:
        explicit SimpleContextSource(std::size_t hard_limit = 0)
            : m_hard_limit(hard_limit), m_used(0) {}

        auto token_count() const -> std::size_t override { return m_used; }
        auto add_tokens(std::size_t n) -> void override { m_used += n; }
        auto would_exceed_hard_limit(std::size_t additional) const -> bool override
        {
            return m_hard_limit > 0 && (m_used + additional) > m_hard_limit;
        }
        auto hard_limit() const -> std::size_t override { return m_hard_limit; }

        // ─── 扩展接口 ───

        // 设置硬限（context window 大小）
        auto set_hard_limit(std::size_t limit) -> void { m_hard_limit = limit; }

        // 重置计数
        auto reset() -> void { m_used = 0; }

        // 估算剩余可用 token 数
        auto remaining() const -> std::size_t
        {
            if (m_hard_limit == 0 || m_used >= m_hard_limit)
                return 0;
            return m_hard_limit - m_used;
        }

        // 使用率（0.0 ~ 1.0）
        auto usage_ratio() const -> double override
        {
            if (m_hard_limit == 0)
                return 0.0;
            return static_cast<double>(m_used) / static_cast<double>(m_hard_limit);
        }

        // 安全预留比例（返回安全的 max_completion_tokens，预留缓冲）
        // buffer_ratio: 预留比例，默认 0.1（10% 缓冲）
        auto safe_completion_tokens(double buffer_ratio = 0.1) const -> std::size_t
        {
            if (m_hard_limit == 0)
                return 2048; // 默认值
            if (m_used >= m_hard_limit)
                return 0;
            std::size_t avail = m_hard_limit - m_used;
            auto buffered = static_cast<std::size_t>(static_cast<double>(avail) * (1.0 - buffer_ratio));
            // 用 (std::max) 避免 Windows.h 的 max 宏干扰
            return (std::max)(buffered, std::size_t{64});
        }

    private:
        std::size_t m_hard_limit;
        std::size_t m_used = 0;
    };

    // ---------------------------------------------------- SimpleToolProvider
    // 本地工具注册表（lambda 式）；生产环境由宿主适配器替换。
    class SimpleToolProvider : public IToolProvider
    {
    public:
        using ToolFn = std::function<Core::Result<nlohmann::json>(const nlohmann::json &args)>;
        auto register_tool(std::string name, ToolFn fn) -> void
        {
            m_tools[std::move(name)] = std::move(fn);
        }
        auto execute(std::string_view name,
                     const nlohmann::json &args,
                     int /*max_retries*/ = 3) -> Core::Result<nlohmann::json> override
        {
            auto it = m_tools.find(std::string{name});
            if (it == m_tools.end())
                return Core::Result<nlohmann::json>::Error(
                    Core::Error{"ToolNotFound: " + std::string{name}});
            try
            {
                return it->second(args);
            }
            catch (const std::exception &e)
            {
                return Core::Result<nlohmann::json>::Error(
                    Core::Error{std::string{"ToolExec: "} + e.what()});
            }
        }
        auto list_tools() const -> std::vector<std::string> override
        {
            std::vector<std::string> out;
            for (auto &kv : m_tools)
                out.push_back(kv.first);
            return out;
        }
        // 装载本地工具的 OpenAI 兼容 schema（供 LLM tool_choice 与请求体组装）。
        void set_tools_schema(nlohmann::json schema) { m_schema = std::move(schema); }
        auto get_tools_schema() const -> nlohmann::json override { return m_schema; }

    private:
        std::unordered_map<std::string, ToolFn> m_tools;
        nlohmann::json m_schema = nlohmann::json::array();
    };

    // ---------------------------------------------------- SimpleStateStore
    class SimpleStateStore : public IStateStore
    {
    public:
        auto save(std::string_view session_id, std::string_view snapshot) -> Core::Result<void> override
        {
            std::scoped_lock lock{m_mu};
            m_data[std::string{session_id}] = std::string{snapshot};
            return Core::Result<void>::Ok();
        }

        auto load(std::string_view session_id) -> Core::Result<std::string> override
        {
            std::scoped_lock lock{m_mu};
            auto it = m_data.find(std::string{session_id});
            if (it == m_data.end())
                return Core::Result<std::string>::Ok(std::string{});
            return Core::Result<std::string>::Ok(it->second);
        }

        auto contains(std::string_view session_id) const -> bool
        {
            std::scoped_lock lock{m_mu};
            return m_data.contains(std::string{session_id});
        }

        auto clear() -> void
        {
            std::scoped_lock lock{m_mu};
            m_data.clear();
        }

        auto remove(std::string_view session_id) -> void
        {
            std::scoped_lock lock{m_mu};
            m_data.erase(std::string{session_id});
        }

    private:
        mutable std::mutex m_mu;
        std::unordered_map<std::string, std::string> m_data;
    };

} // namespace AE::Engine
