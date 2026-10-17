#pragma once

/// @file Ports/InMemoryStore.hpp
/// @brief 记忆后端参考实现：`IMemoryStore`（KV + TTL + 关键词打分召回）+ 三层适配
///        （Docs/01 §7.2、§8.1 P7）。
///
/// @details
/// - `InMemoryStore`：进程内 KV，按 `scope` 分桶；`upsert` 去重；`query` 以词元重叠打分
///   （确定性、零依赖，可平滑替换为向量库后端）；`expire` 按 `created_at` 清理 TTL。
/// - 适配器：`LongTermMemoryOnStore`（`ILongTermMemory`）、`SharedMemoryOnStore`
///   （`ISharedMemory`）、`WorkingMemoryWindow`（`IWorkingMemory`，工作记忆窗口）。
/// - 线程安全：内部互斥（记忆写入可能来自多协程/线程）。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Error.hpp"

#include "Ports/MemoryPort.hpp"

namespace AE::Engine
{

    // ---------------------------------------------------- 词元打分（确定性，无第三方依赖）
    namespace detail
    {
        [[nodiscard]] inline auto tokenize(std::string_view s) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            std::string cur;
            for (char c : s)
            {
                const auto uc = static_cast<unsigned char>(c);
                if (std::isalnum(uc))
                {
                    cur.push_back(static_cast<char>(std::tolower(uc)));
                }
                else if (!cur.empty())
                {
                    out.push_back(std::move(cur));
                    cur.clear();
                }
            }
            if (!cur.empty())
                out.push_back(std::move(cur));
            return out;
        }

        // query 中出现的词元在 text 中命中比例（[0,1]）。
        [[nodiscard]] inline auto overlap_score(std::string_view query, std::string_view text) -> double
        {
            const auto q = tokenize(query);
            if (q.empty())
                return 0.0;
            const auto t = tokenize(text);
            std::size_t hit = 0;
            for (const auto &w : q)
                if (std::find(t.begin(), t.end(), w) != t.end())
                    ++hit;
            return static_cast<double>(hit) / static_cast<double>(q.size());
        }
    } // namespace detail

    // ---------------------------------------------------- KV / 打分后端
    class InMemoryStore final : public IMemoryStore
    {
    public:
        explicit InMemoryStore(MemoryWriteMode mode = MemoryWriteMode::WriteThrough) : m_mode(mode) {}

        auto upsert(std::string_view scope, MemoryRecord r) -> Core::Result<void> override
        {
            std::scoped_lock lk{m_mu};
            if (r.id.empty())
                r.id = "mem_" + std::to_string(++m_seq);
            if (r.scope.empty())
                r.scope = std::string{scope};
            if (r.created_at == std::chrono::system_clock::time_point{})
                r.created_at = std::chrono::system_clock::now();

            auto &bucket = m_data[std::string{scope}];
            for (auto &e : bucket)
                if (e.id == r.id)
                {
                    e = std::move(r);
                    return Core::Result<void>::Ok();
                }
            bucket.push_back(std::move(r));
            return Core::Result<void>::Ok();
        }

        auto query(std::string_view scope, std::string_view q, std::size_t k)
            -> Core::Result<std::vector<MemoryRecord>> override
        {
            std::scoped_lock lk{m_mu};
            std::vector<MemoryRecord> out;
            auto it = m_data.find(std::string{scope});
            if (it == m_data.end())
                return Core::Result<std::vector<MemoryRecord>>::Ok(std::move(out));

            std::vector<std::pair<double, const MemoryRecord *>> scored;
            scored.reserve(it->second.size());
            for (const auto &e : it->second)
            {
                const double s = q.empty() ? 0.0 : detail::overlap_score(q, e.text);
                if (q.empty() || s > 0.0)
                    scored.emplace_back(s, &e);
            }
            std::sort(scored.begin(), scored.end(),
                      [](const auto &a, const auto &b)
                      {
                          if (a.first != b.first)
                              return a.first > b.first;
                          return a.second->created_at > b.second->created_at;
                      });
            for (std::size_t i = 0; i < scored.size() && i < k; ++i)
                out.push_back(*scored[i].second);
            return Core::Result<std::vector<MemoryRecord>>::Ok(std::move(out));
        }

        auto expire(std::string_view scope, std::chrono::seconds ttl) -> Core::Result<std::size_t> override
        {
            std::scoped_lock lk{m_mu};
            auto it = m_data.find(std::string{scope});
            if (it == m_data.end())
                return Core::Result<std::size_t>::Ok(0);

            const auto cutoff = std::chrono::system_clock::now() - ttl;
            auto &vec = it->second;
            const auto before = vec.size();
            std::erase_if(vec, [&](const MemoryRecord &e) { return e.created_at < cutoff; });
            return Core::Result<std::size_t>::Ok(before - vec.size());
        }

        [[nodiscard]] auto size(std::string_view scope) const -> std::size_t
        {
            std::scoped_lock lk{m_mu};
            auto it = m_data.find(std::string{scope});
            return it == m_data.end() ? 0 : it->second.size();
        }

        [[nodiscard]] auto write_mode() const noexcept -> MemoryWriteMode { return m_mode; }

    private:
        mutable std::mutex m_mu;
        std::unordered_map<std::string, std::vector<MemoryRecord>> m_data;
        MemoryWriteMode m_mode;
        std::uint64_t m_seq{0};
    };

    // ---------------------------------------------------- 长期记忆适配
    class LongTermMemoryOnStore final : public ILongTermMemory
    {
    public:
        LongTermMemoryOnStore(IMemoryStore &store, std::string scope)
            : m_store(store), m_scope(std::move(scope)) {}

        auto recall(std::string_view query, std::size_t k)
            -> Core::Result<std::vector<MemoryRecord>> override
        {
            return m_store.query(m_scope, query, k);
        }

        auto remember(MemoryRecord r) -> Core::Result<void> override
        {
            if (r.scope.empty())
                r.scope = m_scope;
            return m_store.upsert(m_scope, std::move(r));
        }

    private:
        IMemoryStore &m_store;
        std::string m_scope;
    };

    // ---------------------------------------------------- 共享记忆适配
    class SharedMemoryOnStore final : public ISharedMemory
    {
    public:
        explicit SharedMemoryOnStore(IMemoryStore &store) : m_store(store) {}

        auto get(std::string_view scope, std::string_view key) -> Core::Result<nlohmann::json> override
        {
            // 后端无「按 id 直取」抽象：以空查询取回该 scope 全量后按 id 匹配（参考实现）。
            auto r = m_store.query(scope, {}, (std::numeric_limits<std::size_t>::max)());
            if (r.isError())
                return Core::Result<nlohmann::json>::Error(r.error());
            for (const auto &rec : r.value())
                if (rec.id == key)
                    return Core::Result<nlohmann::json>::Ok(rec.meta);
            return Core::Result<nlohmann::json>::Error(
                Core::Error(Core::ErrorCode::NotFound, "shared memory key not found: " + std::string{key}));
        }

        auto put(std::string_view scope, std::string_view key, nlohmann::json v) -> Core::Result<void> override
        {
            MemoryRecord r;
            r.id = std::string{key};
            r.scope = std::string{scope};
            r.text = v.dump();
            r.meta = std::move(v);
            return m_store.upsert(scope, std::move(r));
        }

    private:
        IMemoryStore &m_store;
    };

    // ---------------------------------------------------- 工作记忆窗口（当前会话）
    class WorkingMemoryWindow final : public IWorkingMemory
    {
    public:
        explicit WorkingMemoryWindow(std::size_t max_messages = 64) : m_max(max_messages) {}

        void push(std::string_view session_id, nlohmann::json msg)
        {
            std::scoped_lock lk{m_mu};
            auto &win = m_windows[std::string{session_id}];
            win.push_back(std::move(msg));
            while (win.size() > m_max)
                win.erase(win.begin());
        }

        [[nodiscard]] auto window(std::string_view session_id)
            -> std::span<const nlohmann::json> override
        {
            std::scoped_lock lk{m_mu};
            auto it = m_windows.find(std::string{session_id});
            if (it == m_windows.end() || it->second.empty())
                return {};
            return {it->second.data(), it->second.size()};
        }

        auto summarize(std::string_view session_id) -> Core::Result<std::string> override
        {
            std::scoped_lock lk{m_mu};
            auto it = m_windows.find(std::string{session_id});
            const std::size_t n = it == m_windows.end() ? 0 : it->second.size();
            return Core::Result<std::string>::Ok(
                "working memory: " + std::to_string(n) + " messages, last " + std::to_string(m_max) + " kept");
        }

    private:
        mutable std::mutex m_mu;
        std::unordered_map<std::string, std::vector<nlohmann::json>> m_windows;
        std::size_t m_max;
    };

} // namespace AE::Engine
