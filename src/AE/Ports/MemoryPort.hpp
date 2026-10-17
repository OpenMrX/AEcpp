#pragma once

/// @file Ports/MemoryPort.hpp
/// @brief 记忆 Port：三层（工作 / 长期 / 共享）+ 后端抽象（Docs/01 §7.2）。
///
/// @details
/// 设计约束（关键）：
/// - 只依赖 `Core::Result` / `nlohmann::json` / 标准库。
/// - 消息载体沿用引擎既有表示 `nlohmann::json`（而非独立 Message 结构），保证零回归。
/// - 治理：写入策略（WriteThrough / WriteBehind）、scope 隔离、TTL 清理由实现负责。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"

namespace AE::Engine
{

    // ---------------------------------------------------- 记忆记录
    struct MemoryRecord
    {
        std::string id;
        std::string scope;                        // 隔离域（按人 / 按线程）
        std::string text;
        nlohmann::json meta = nlohmann::json::object();
        std::chrono::system_clock::time_point created_at{};
    };

    // 写入策略（治理，§四.1）。
    enum class MemoryWriteMode : std::uint8_t
    {
        WriteThrough, // 同步写穿（强一致）
        WriteBehind   // 异步写回（低延迟，最终一致）
    };

    // ---------------------------------------------------- 短期：工作记忆
    // 当前会话上下文，受 token 窗口约束；与引擎的 IContextSource 协作（计数分离）。
    class IWorkingMemory
    {
    public:
        virtual ~IWorkingMemory() = default;
        // 当前会话可见的消息窗口（只读视图；消息载体与引擎一致为 json）。
        [[nodiscard]] virtual auto window(std::string_view session_id)
            -> std::span<const nlohmann::json> = 0;
        // 生成滚动摘要（供软限压缩）。
        virtual auto summarize(std::string_view session_id) -> Core::Result<std::string> = 0;
    };

    // ---------------------------------------------------- 长期：跨会话画像/经验
    class ILongTermMemory
    {
    public:
        virtual ~ILongTermMemory() = default;
        // 语义/关键词召回 top-k。
        virtual auto recall(std::string_view query, std::size_t k)
            -> Core::Result<std::vector<MemoryRecord>> = 0;
        virtual auto remember(MemoryRecord r) -> Core::Result<void> = 0;
    };

    // ---------------------------------------------------- 共享：多 Agent 一致性上下文
    class ISharedMemory
    {
    public:
        virtual ~ISharedMemory() = default;
        virtual auto get(std::string_view scope, std::string_view key)
            -> Core::Result<nlohmann::json> = 0;
        virtual auto put(std::string_view scope, std::string_view key, nlohmann::json v)
            -> Core::Result<void> = 0;
    };

    // ---------------------------------------------------- 后端：KV / 向量库抽象
    class IMemoryStore
    {
    public:
        virtual ~IMemoryStore() = default;
        virtual auto upsert(std::string_view scope, MemoryRecord r) -> Core::Result<void> = 0;
        virtual auto query(std::string_view scope, std::string_view q, std::size_t k)
            -> Core::Result<std::vector<MemoryRecord>> = 0;
        // 清理超过 ttl 的记录，返回清理条数。
        virtual auto expire(std::string_view scope, std::chrono::seconds ttl)
            -> Core::Result<std::size_t> = 0;
    };

} // namespace AE::Engine
