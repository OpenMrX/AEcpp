#pragma once

/// @file AESessionStore.hpp
/// @brief 会话 JSON 持久化存储（原子写入 temp+rename）。
///
/// @details
/// 从 React+Session 迁移，适配 AE::IStateStore 接口。
///
/// 设计：
/// - AESessionStore 实现 IStateStore 接口（save/load），使 AgentEngine 可
/// 直接持久化会话快照到磁盘。
/// - 底层存储：每个会话一个 .json 文件，原子写入（temp file + rename）。
/// - 线程安全：std::mutex 保护文件 I/O。
///
/// @note 所属命名空间：AE::Engine
/// @note 依赖：AEType.hpp（IStateStore）、nlohmann::json、Core::Result。

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "AEType.hpp"

namespace AE::Engine
{

    // ================================================================
    //  SessionInfo — 会话元数据
    // ================================================================

    struct SessionInfo
    {
        std::string id;
        std::string timestamp;
        std::size_t message_count{0};
    };

    // ================================================================
    //  AESessionStore — IStateStore 实现
    // ================================================================

    class AESessionStore : public IStateStore
    {
    public:
        AESessionStore();
        explicit AESessionStore(std::string_view dir);
        ~AESessionStore() override = default;

        AESessionStore(const AESessionStore&) = delete;
        AESessionStore& operator=(const AESessionStore&) = delete;

        // ---- IStateStore 接口 ----
        auto save(std::string_view session_id,
            std::string_view snapshot) -> Core::Result<void> override;
        auto load(std::string_view session_id)
            -> Core::Result<std::string> override;

        // ---- 扩展接口 ----
        [[nodiscard]] auto exists(std::string_view session_id) const -> bool;
        [[nodiscard]] auto list() const -> std::vector<SessionInfo>;
        void remove(std::string_view session_id);

        [[nodiscard]] static auto generateId() -> std::string;

    private:
        [[nodiscard]] auto pathFor(std::string_view id) const -> std::string;
        static auto validateId(std::string_view id) -> Core::Result<void>;

        std::string dir_;
        mutable std::mutex mutex_;
    };

} // namespace AE::Engine
