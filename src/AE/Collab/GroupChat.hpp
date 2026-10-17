#pragma once

/// @file Collab/GroupChat.hpp
/// @brief 协作模式 §二.3：GroupChat（共享会话，动态发言）(Docs/01 §5.3)。
///
/// @details 参与者轮次发言、共享会话历史；发言者由 `ISpeakerSelector` 选择
///          （默认轮询）。终局可由 Concluder 汇总，否则取末条发言。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <cstdint>
#include <generator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Collab/ICollaboration.hpp"
#include "Collab/AgentRuntime.hpp"
#include "Collab/AgentRegistry.hpp"
#include "Collab/MessageBus.hpp"

namespace AE::Engine
{

    // 发言者选择（每轮一次调用，非热路径）。
    class ISpeakerSelector
    {
    public:
        virtual ~ISpeakerSelector() = default;
        virtual auto select(std::uint32_t round,
                            const std::vector<AgentMessage> &history,
                            CollabContext &ctx) -> std::string = 0;
    };

    class RoundRobinSpeakerSelector final : public ISpeakerSelector
    {
    public:
        auto select(std::uint32_t round, const std::vector<AgentMessage> &,
                    CollabContext &ctx) -> std::string override
        {
            const auto &agents = ctx.registry.all();
            if (agents.empty())
                return {};
            return agents[round % agents.size()]->spec().id;
        }
    };

    struct GroupChatConfig
    {
        std::vector<std::string> participant_ids; // 空 = 注册表全部
        std::string concluder_id;                 // 可选：终局汇总者
        std::uint32_t max_rounds = 6;
    };

    class GroupChat final : public ICollaboration
    {
    public:
        explicit GroupChat(GroupChatConfig cfg = {},
                           std::unique_ptr<ISpeakerSelector> selector = nullptr)
            : m_cfg(std::move(cfg)),
              m_selector(selector ? std::move(selector) : std::make_unique<RoundRobinSpeakerSelector>()) {}

        [[nodiscard]] auto name() const -> std::string_view override { return "group_chat"; }

        auto run(CollabContext &ctx) -> std::generator<CollaborationEvent> override
        {
            co_yield CollaborationEvent::collab_started(std::string{name()});

            std::vector<AgentMessage> history;
            AgentMessage seed;
            seed.from = "human";
            seed.to = "*";
            seed.kind = AgentMessage::Kind::Task;
            seed.content = ctx.task.is_string() ? ctx.task.get<std::string>() : ctx.task.dump();
            ctx.bus.send(seed);
            history.push_back(seed);
            co_yield CollaborationEvent::message_sent(seed);

            std::string last;
            for (std::uint32_t round = 0; round < m_cfg.max_rounds; ++round)
            {
                const std::string id = m_selector->select(round, history, ctx);
                IAgentRuntime *a = ctx.registry.get(id);
                if (!a)
                    continue;

                AgentMessage turn;
                turn.from = id;
                turn.to = "*";
                turn.kind = AgentMessage::Kind::Info;
                turn.content = render_history(history);

                std::string out;
                for (auto &&ce : drive_agent(*a, turn, ctx, out))
                    co_yield ce;

                AgentMessage reply;
                reply.from = id;
                reply.to = "*";
                reply.kind = AgentMessage::Kind::Result;
                reply.content = out;
                reply.reply_to = turn.id;
                ctx.bus.send(reply);
                history.push_back(reply);
                last = out;
                co_yield CollaborationEvent::message_sent(reply);
            }

            std::string final = last;
            if (!m_cfg.concluder_id.empty())
            {
                if (IAgentRuntime *c = ctx.registry.get(m_cfg.concluder_id))
                {
                    AgentMessage concl;
                    concl.from = "human";
                    concl.to = m_cfg.concluder_id;
                    concl.kind = AgentMessage::Kind::Task;
                    concl.content = "请基于以下讨论给出最终结论：\n" + render_history(history);
                    ctx.bus.send(concl);
                    for (auto &&ce : drive_agent(*c, concl, ctx, final))
                        co_yield ce;
                }
            }

            co_yield CollaborationEvent::consensus(final);
            co_yield CollaborationEvent::collab_finished(final);
        }

    private:
        static auto render_history(const std::vector<AgentMessage> &history) -> std::string
        {
            std::string s;
            for (const auto &m : history)
            {
                s += '[';
                s += m.from;
                s += "] ";
                s += m.content;
                s += '\n';
            }
            return s;
        }

        GroupChatConfig m_cfg;
        std::unique_ptr<ISpeakerSelector> m_selector;
    };

} // namespace AE::Engine
