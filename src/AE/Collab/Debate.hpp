#pragma once

/// @file Collab/Debate.hpp
/// @brief 协作模式 §二.4：Debate（多视角论证，收敛判定）(Docs/01 §5.4)。
///
/// @details 多辩手对同一问题各自论证，按 `IConvergencePolicy` 判定是否收敛
///          （默认：出现两条内容相同的发言即收敛）；硬上限 `hard_cap` 兜底；
///          可选 Moderator 给出裁决。复用 `Critique` 表达观点/理由/证据。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <algorithm>
#include <cstdint>
#include <deque>
#include <generator>
#include <memory>
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

    class IConvergencePolicy
    {
    public:
        virtual ~IConvergencePolicy() = default;
        virtual auto converged(std::uint32_t round,
                               const std::vector<AgentMessage> &round_msgs,
                               const std::deque<std::string> &all_outputs) -> bool = 0;
    };

    // 默认：本轮出现两条内容相同的发言视为收敛。
    class SimilarityConvergencePolicy final : public IConvergencePolicy
    {
    public:
        auto converged(std::uint32_t, const std::vector<AgentMessage> &round_msgs,
                       const std::deque<std::string> &) -> bool override
        {
            std::unordered_set<std::string> seen;
            for (const auto &m : round_msgs)
            {
                if (!seen.insert(m.content).second)
                    return true;
            }
            return false;
        }
    };

    struct DebateConfig
    {
        std::vector<std::string> debater_ids;
        std::string moderator_id; // 可选
        std::uint32_t max_rounds = 3;   // 收敛软上限
        std::uint32_t hard_cap = 5;     // 绝对上限（§5.4）
    };

    class Debate final : public ICollaboration
    {
    public:
        explicit Debate(DebateConfig cfg = {},
                        std::unique_ptr<IConvergencePolicy> policy = nullptr)
            : m_cfg(std::move(cfg)),
              m_policy(policy ? std::move(policy) : std::make_unique<SimilarityConvergencePolicy>()) {}

        [[nodiscard]] auto name() const -> std::string_view override { return "debate"; }

        auto run(CollabContext &ctx) -> std::generator<CollaborationEvent> override
        {
            co_yield CollaborationEvent::collab_started(std::string{name()});
            const std::string task_text = ctx.task.is_string() ? ctx.task.get<std::string>() : ctx.task.dump();

            const std::uint32_t cap = (std::max)(m_cfg.max_rounds, m_cfg.hard_cap);
            std::deque<std::string> all_outputs;
            bool done = false;

            for (std::uint32_t round = 0; round < cap; ++round)
            {
                std::vector<AgentMessage> round_msgs;
                for (const auto &id : m_cfg.debater_ids)
                {
                    IAgentRuntime *a = ctx.registry.get(id);
                    if (!a)
                    {
                        co_yield CollaborationEvent::collab_error("debater not found: " + id);
                        continue;
                    }

                    AgentMessage msg;
                    msg.from = "human";
                    msg.to = id;
                    msg.kind = AgentMessage::Kind::Task;
                    msg.content = "辩题：" + task_text + "\n\n请给出你的论证（观点/理由/证据）。" +
                                  (round > 0 ? "\n可参考此前发言并改进。" : "");

                    std::string out;
                    for (auto &&ce : drive_agent(*a, msg, ctx, out))
                        co_yield ce;

                    AgentMessage arg;
                    arg.from = id;
                    arg.to = "*";
                    arg.kind = AgentMessage::Kind::Critique;
                    arg.content = out;
                    arg.reply_to = msg.id;
                    ctx.bus.send(arg);
                    round_msgs.push_back(arg);
                    all_outputs.push_back(out);
                    co_yield CollaborationEvent::message_sent(arg);
                }

                if (m_policy->converged(round, round_msgs, all_outputs) && round + 1 >= m_cfg.max_rounds)
                {
                    done = true;
                    break;
                }
                if (m_policy->converged(round, round_msgs, all_outputs))
                {
                    co_yield CollaborationEvent::consensus("converged at round " + std::to_string(round));
                    done = true;
                    break;
                }
            }
            (void)done;

            std::string verdict;
            if (!m_cfg.moderator_id.empty())
            {
                if (IAgentRuntime *mod = ctx.registry.get(m_cfg.moderator_id))
                {
                    AgentMessage vmsg;
                    vmsg.from = "human";
                    vmsg.to = m_cfg.moderator_id;
                    vmsg.kind = AgentMessage::Kind::Task;
                    vmsg.content = "请裁决以下辩论并给出最终结论：\n";
                    for (const auto &o : all_outputs)
                    {
                        vmsg.content += "- ";
                        vmsg.content += o;
                        vmsg.content += '\n';
                    }
                    ctx.bus.send(vmsg);
                    for (auto &&ce : drive_agent(*mod, vmsg, ctx, verdict))
                        co_yield ce;
                }
            }

            co_yield CollaborationEvent::consensus(verdict);
            co_yield CollaborationEvent::collab_finished(verdict);
        }

    private:
        DebateConfig m_cfg;
        std::unique_ptr<IConvergencePolicy> m_policy;
    };

} // namespace AE::Engine
