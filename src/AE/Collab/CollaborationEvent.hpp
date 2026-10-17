#pragma once

/// @file Collab/CollaborationEvent.hpp
/// @brief 协作层消息 + 事件 IR（独立类型，评审结论 #3）（Docs/01 §5.0.2）。
///
/// @details
/// - `AgentMessage`：协作内消息（点对点/广播），含结构化载荷与时间戳。
/// - `CollaborationEvent`：**独立**于单 Agent IR 的协作事件；内嵌的 `AgentEvent` 仅作透传，
///   不污染单 Agent 事件流（评审结论 #3）。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "nlohmann-json.hpp"

#include "AEType.hpp" // AgentEvent

namespace AE::Engine
{

    struct AgentMessage
    {
        std::string id;
        std::string from; // 发送方 agent id（或 "human"）
        std::string to;   // 目标 id / "*" 广播
        enum class Kind : std::uint8_t
        {
            Task,
            Result,
            Critique,
            Vote,
            Handoff,
            Info
        };
        Kind kind{Kind::Info};
        std::string content;
        nlohmann::json data; // 结构化载荷（计划/打分/证据）
        std::string reply_to;
        std::chrono::system_clock::time_point ts{};

        [[nodiscard]] static auto kind_name(Kind k) noexcept -> std::string_view
        {
            switch (k)
            {
            case Kind::Task:
                return "Task";
            case Kind::Result:
                return "Result";
            case Kind::Critique:
                return "Critique";
            case Kind::Vote:
                return "Vote";
            case Kind::Handoff:
                return "Handoff";
            case Kind::Info:
                return "Info";
            }
            return "Info";
        }
    };

    struct CollaborationEvent
    {
        enum class Kind : std::uint8_t
        {
            CollabStarted,
            AgentStarted,
            AgentFinished,
            MessageSent,
            Handoff,
            Vote,
            Consensus,
            CollabError,
            CollabFinished
        };
        Kind kind{Kind::CollabStarted};
        std::string agent_id;
        std::optional<AgentMessage> message;
        std::optional<AgentEvent> inner; // 内嵌单 Agent 事件（透传，不污染）
        std::string detail;

        // ---- 工厂（编译期枚举 + 便捷构造） ----
        static auto collab_started(std::string detail = {}) -> CollaborationEvent
        {
            return CollaborationEvent{Kind::CollabStarted, {}, std::nullopt, std::nullopt, std::move(detail)};
        }
        static auto agent_started(std::string id) -> CollaborationEvent
        {
            return CollaborationEvent{Kind::AgentStarted, std::move(id), std::nullopt, std::nullopt, {}};
        }
        static auto agent_finished(std::string id, std::string detail = {}) -> CollaborationEvent
        {
            return CollaborationEvent{Kind::AgentFinished, std::move(id), std::nullopt, std::nullopt, std::move(detail)};
        }
        static auto message_sent(AgentMessage m) -> CollaborationEvent
        {
            CollaborationEvent e;
            e.kind = Kind::MessageSent;
            e.agent_id = m.from;
            e.message = std::move(m);
            return e;
        }
        static auto inner_event(std::string id, AgentEvent ev) -> CollaborationEvent
        {
            CollaborationEvent e;
            e.kind = Kind::AgentStarted;
            e.agent_id = std::move(id);
            e.inner = std::move(ev);
            return e;
        }
        static auto vote(std::string id, std::string detail) -> CollaborationEvent
        {
            return CollaborationEvent{Kind::Vote, std::move(id), std::nullopt, std::nullopt, std::move(detail)};
        }
        static auto consensus(std::string detail) -> CollaborationEvent
        {
            return CollaborationEvent{Kind::Consensus, {}, std::nullopt, std::nullopt, std::move(detail)};
        }
        static auto handoff(std::string id, AgentMessage m) -> CollaborationEvent
        {
            CollaborationEvent e;
            e.kind = Kind::Handoff;
            e.agent_id = std::move(id);
            e.message = std::move(m);
            return e;
        }
        static auto collab_error(std::string detail) -> CollaborationEvent
        {
            return CollaborationEvent{Kind::CollabError, {}, std::nullopt, std::nullopt, std::move(detail)};
        }
        static auto collab_finished(std::string detail = {}) -> CollaborationEvent
        {
            return CollaborationEvent{Kind::CollabFinished, {}, std::nullopt, std::nullopt, std::move(detail)};
        }
    };

} // namespace AE::Engine
