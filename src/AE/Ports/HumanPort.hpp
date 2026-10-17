#pragma once

/// @file Ports/HumanPort.hpp
/// @brief 人机协作 Port：确认门 / 中断 / 接管（Docs/01 §7.4）。
///
/// @details
/// 设计约束（关键）：
/// - 只依赖 `Core::Result` / 标准库。「人类」可被包装为一个 Agent 运行时（from="human"），
///   供协作层（§5.2/§5.3）复用。
/// - `NullHumanGate` 为默认直通实现（全部放行），便于无人工场景。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <cstdint>
#include <string>
#include <string_view>

#include "Core/Result.hpp"

namespace AE::Engine
{

    // ---------------------------------------------------- 审批请求
    struct ApprovalRequest
    {
        std::string action;   // 待审批动作（如 "tool.exec:delete_file"）
        std::string detail;   // 人类可读详情
        bool destructive = false;
    };

    // ---------------------------------------------------- 人机门
    class IHumanGate
    {
    public:
        enum class Decision : std::uint8_t
        {
            Approve,  // 放行
            Reject,   // 拒绝
            Modify,   // 人类改写后放行（改写内容经 detail 回传，由调用方约定）
            Takeover  // 人类接管控制权
        };

        virtual ~IHumanGate() = default;
        virtual auto request_approval(const ApprovalRequest &req) -> Core::Result<Decision> = 0;
        virtual auto interrupt(std::string_view reason) -> void = 0;
        virtual auto resume() -> void = 0;
        virtual auto takeover(std::string_view session_id) -> Core::Result<void> = 0;
    };

    // 直通实现：全部放行（无人值守 / 测试）。
    class NullHumanGate final : public IHumanGate
    {
    public:
        auto request_approval(const ApprovalRequest &) -> Core::Result<Decision> override
        {
            return Core::Result<Decision>::Ok(Decision::Approve);
        }
        auto interrupt(std::string_view) -> void override {}
        auto resume() -> void override {}
        auto takeover(std::string_view) -> Core::Result<void> override
        {
            return Core::Result<void>::Ok();
        }
    };

} // namespace AE::Engine
