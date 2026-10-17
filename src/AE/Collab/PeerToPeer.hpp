#pragma once

/// @file Collab/PeerToPeer.hpp
/// @brief 协作模式 §二.5：Peer-to-Peer（去中心，去重 + 跳数限制）(Docs/01 §5.5)。
///
/// @details 本节点（`local_id`）从消息总线收取寻址到自己的消息，处理后回发下一跳，
///          按消息 id 去重，`max_hops` 硬上限。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <cstdint>
#include <deque>
#include <generator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Collab/ICollaboration.hpp"
#include "Collab/AgentRuntime.hpp"
#include "Collab/AgentRegistry.hpp"
#include "Collab/MessageBus.hpp"

namespace AE::Engine
{

    struct PeerToPeerConfig
    {
        std::string local_id;
        std::uint32_t max_hops = 4;
        std::string done_token{"[DONE]"};
    };

    class PeerToPeer final : public ICollaboration
    {
    public:
        explicit PeerToPeer(PeerToPeerConfig cfg = {}) : m_cfg(std::move(cfg)) {}

        [[nodiscard]] auto name() const -> std::string_view override { return "peer_to_peer"; }

        auto run(CollabContext &ctx) -> std::generator<CollaborationEvent> override
        {
            co_yield CollaborationEvent::collab_started(std::string{name()});
            IAgentRuntime *local = ctx.registry.get(m_cfg.local_id);
            if (!local)
            {
                co_yield CollaborationEvent::collab_error("local agent not found: " + m_cfg.local_id);
                co_yield CollaborationEvent::collab_finished();
                co_return;
            }

            // 播种：若邮箱为空，则以任务作为首条入站消息。
            std::deque<AgentMessage> pending;
            auto inbox = ctx.bus.mailbox(m_cfg.local_id);
            for (const auto &m : inbox)
                pending.push_back(m);
            if (pending.empty())
            {
                AgentMessage seed;
                seed.from = "human";
                seed.to = m_cfg.local_id;
                seed.kind = AgentMessage::Kind::Task;
                seed.content = ctx.task.is_string() ? ctx.task.get<std::string>() : ctx.task.dump();
                ctx.bus.send(seed);
                pending.push_back(seed);
            }

            std::unordered_set<std::string> seen;
            std::string last;
            for (std::uint32_t hop = 0; hop < m_cfg.max_hops && !pending.empty(); ++hop)
            {
                AgentMessage in = pending.front();
                pending.pop_front();
                if (!in.id.empty() && !seen.insert(in.id).second)
                    continue;

                std::string out;
                for (auto &&ce : drive_agent(*local, in, ctx, out))
                    co_yield ce;

                AgentMessage reply;
                reply.from = m_cfg.local_id;
                reply.to = next_peer(ctx, in.from);
                reply.kind = AgentMessage::Kind::Handoff;
                reply.content = out;
                reply.reply_to = in.id;
                ctx.bus.send(reply);
                co_yield CollaborationEvent::handoff(m_cfg.local_id, reply);
                last = out;

                if (!m_cfg.done_token.empty() && out.find(m_cfg.done_token) != std::string::npos)
                    break;
                if (reply.to.empty() || reply.to == m_cfg.local_id)
                    break;
            }

            co_yield CollaborationEvent::consensus(last);
            co_yield CollaborationEvent::collab_finished(last);
        }

    private:
        [[nodiscard]] auto next_peer(CollabContext &ctx, const std::string &from) const -> std::string
        {
            for (auto *a : ctx.registry.all())
                if (a->spec().id != m_cfg.local_id && a->spec().id != from)
                    return a->spec().id;
            return {};
        }

        PeerToPeerConfig m_cfg;
    };

} // namespace AE::Engine
