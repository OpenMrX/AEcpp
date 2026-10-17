#pragma once

/// @file Ports/ScriptedHumanGate.hpp
/// @brief 人机门参考实现：可脚本化决策队列（Docs/01 §7.4、§8.1 P7）。
///
/// @details
/// - `ScriptedHumanGate`：以预置决策队列模拟人类；队列耗尽后回落默认策略（默认放行）。
///   记录全部审批请求与决策日志，便于测试「审批拒绝路径」与端到端 PoC。
/// - 线程安全：内部互斥。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Result.hpp"

#include "Ports/HumanPort.hpp"

namespace AE::Engine
{

    class ScriptedHumanGate final : public IHumanGate
    {
    public:
        explicit ScriptedHumanGate(Decision default_decision = Decision::Approve)
            : m_default(default_decision) {}

        // 入队一个决策，按 FIFO 与请求配对。
        void enqueue(Decision d)
        {
            std::scoped_lock lk{m_mu};
            m_queue.push_back(d);
        }

        void set_default(Decision d)
        {
            std::scoped_lock lk{m_mu};
            m_default = d;
        }

        auto request_approval(const ApprovalRequest &req) -> Core::Result<Decision> override
        {
            std::scoped_lock lk{m_mu};
            Decision d = m_default;
            if (!m_queue.empty())
            {
                d = m_queue.front();
                m_queue.pop_front();
            }
            m_requests.push_back({req.action, req.destructive, d});
            return Core::Result<Decision>::Ok(d);
        }

        auto interrupt(std::string_view reason) -> void override
        {
            std::scoped_lock lk{m_mu};
            m_interrupted = true;
            m_interrupt_reason = std::string{reason};
        }

        auto resume() -> void override
        {
            std::scoped_lock lk{m_mu};
            m_interrupted = false;
        }

        auto takeover(std::string_view session_id) -> Core::Result<void> override
        {
            std::scoped_lock lk{m_mu};
            m_takeover_session = std::string{session_id};
            m_interrupted = true;
            return Core::Result<void>::Ok();
        }

        // ---- 观测 ----
        struct Entry
        {
            std::string action;
            bool destructive;
            Decision decision;
        };

        [[nodiscard]] auto requests() const -> std::vector<Entry>
        {
            std::scoped_lock lk{m_mu};
            return m_requests;
        }

        [[nodiscard]] auto interrupted() const -> bool
        {
            std::scoped_lock lk{m_mu};
            return m_interrupted;
        }

        [[nodiscard]] auto interrupt_reason() const -> std::string
        {
            std::scoped_lock lk{m_mu};
            return m_interrupt_reason;
        }

        [[nodiscard]] auto takeover_session() const -> std::string
        {
            std::scoped_lock lk{m_mu};
            return m_takeover_session;
        }

    private:
        mutable std::mutex m_mu;
        std::deque<Decision> m_queue;
        Decision m_default;
        std::vector<Entry> m_requests;
        bool m_interrupted{false};
        std::string m_interrupt_reason;
        std::string m_takeover_session;
    };

} // namespace AE::Engine
