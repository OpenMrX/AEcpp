#pragma once

/// @file Mapping/Primitives/Conversation.hpp
/// @brief 对话/群聊原语（AutoGen / MS Agent Framework 隐喻，Docs/01 §6.2、§6.5）。
///
/// @details `Conversation` 维护共享消息历史与共享状态；`ChatDirector` 为中央协调器
///          （默认轮询发言 + 不终止，可派生覆盖）。
///
/// @note 命名空间为扁平 `AE::Engine`（评审结论 #1）。

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann-json.hpp"

#include "Collab/CollaborationEvent.hpp" // AgentMessage
#include "Collab/AgentSpec.hpp"

namespace AE::Engine
{

    class Conversation
    {
    public:
        void push(AgentMessage m) { m_msgs.push_back(std::move(m)); }

        [[nodiscard]] auto context(std::string_view /*for_agent*/) const -> std::span<const AgentMessage>
        {
            return {m_msgs.data(), m_msgs.size()};
        }

        [[nodiscard]] auto messages() const -> std::span<const AgentMessage>
        {
            return {m_msgs.data(), m_msgs.size()};
        }

        [[nodiscard]] auto shared_state() -> nlohmann::json & { return m_state; }

    private:
        std::vector<AgentMessage> m_msgs;
        nlohmann::json m_state = nlohmann::json::object();
    };

    // 中央协调器：选择发言者 + 终止判定。
    class ChatDirector
    {
    public:
        virtual ~ChatDirector() = default;

        virtual auto select_speaker(Conversation & /*conv*/, std::span<const AgentSpec> agents) -> std::size_t
        {
            if (agents.empty())
                return 0;
            return m_turn++ % agents.size();
        }

        [[nodiscard]] virtual auto should_terminate(const Conversation & /*conv*/) const -> bool
        {
            return false;
        }

    private:
        std::size_t m_turn{0};
    };

} // namespace AE::Engine
