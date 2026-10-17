#pragma once

/// @file Collab/MessageBus.hpp
/// @brief 协作内消息传递：点对点 + 广播 + A2A/跨进程适配点（Docs/01 §5.0.3）。
///
/// @details 只依赖 `CollaborationEvent.hpp` 与标准库。`bind_transport` 预留跨进程/A2A
///          通道（§7.1）；未绑定时仅进程内投递。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Core/Result.hpp"
#include "Collab/CollaborationEvent.hpp"

namespace AE::Engine
{

    // A2A / 跨进程传输适配点（可选）。
    class IMessageTransport
    {
    public:
        virtual ~IMessageTransport() = default;
        virtual auto deliver(const AgentMessage &m) -> Core::Result<void> = 0;
    };

    class MessageBus
    {
    public:
        // 投递一条消息：写入目标邮箱 + 追加到全量日志；若绑定传输则外发。
        void send(AgentMessage m)
        {
            if (m.id.empty())
                m.id = "msg_" + std::to_string(++m_seq);
            if (m.ts == std::chrono::system_clock::time_point{})
                m.ts = std::chrono::system_clock::now();
            if (m_transport)
                (void)m_transport->deliver(m); // 外发尽力而为，失败不阻断进程内投递
            m_mailbox[target_key(m.to)].push_back(m);
            m_log.push_back(std::move(m));
        }

        // 读取某 agent 的邮箱（只读视图）。
        [[nodiscard]] auto mailbox(std::string_view agent_id) const -> std::span<const AgentMessage>
        {
            auto it = m_mailbox.find(std::string{agent_id});
            if (it == m_mailbox.end())
                return {};
            return {it->second.data(), it->second.size()};
        }

        // 全量消息日志（按发送顺序）。
        [[nodiscard]] auto log() const -> std::span<const AgentMessage>
        {
            return {m_log.data(), m_log.size()};
        }

        void bind_transport(IMessageTransport *t) { m_transport = t; }
        [[nodiscard]] auto transport() const -> IMessageTransport * { return m_transport; }
        void clear() { m_mailbox.clear(); m_log.clear(); }

    private:
        static auto target_key(const std::string &to) -> std::string
        {
            return to.empty() ? std::string{"*"} : to;
        }

        std::unordered_map<std::string, std::vector<AgentMessage>> m_mailbox;
        std::vector<AgentMessage> m_log;
        IMessageTransport *m_transport = nullptr;
        std::uint64_t m_seq = 0;
    };

} // namespace AE::Engine
