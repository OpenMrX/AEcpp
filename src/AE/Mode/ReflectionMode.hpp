#pragma once

/// @file Mode/ReflectionMode.hpp
/// @brief 反思纠错模式：装饰任意 `ReasoningMode` 的质量控制层（Docs/01 §4.4）。
///
/// @details
/// - **零开销装饰**：内层模式**静态内联**（模板参数），无多态；仅 `IReflector` 边界注入。
/// - 循环：生成 → 评审 →（不达标则注入反馈重生成）→ 达标/触顶/熔断即止。
/// - 无效循环保护（§4.4.4）：`fingerprint` 原地打转（与上轮相同）或 score 连续无进展即熔断，
///   发出 `ReflectionExhausted`，返回当前最佳产出（不抛异常，H15）。
///
/// 组合（§4.4.5）：`ReflectionMode<ReactMode>` / `ReflectionMode<PlanExecuteMode>`。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <cstdint>
#include <generator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann-json.hpp"

#include "AEType.hpp"
#include "Mode/ModeContext.hpp"
#include "Mode/ModeKind.hpp"
#include "Mode/IReasoningMode.hpp"
#include "Mode/IReflector.hpp"

namespace AE::Engine
{

    // 触发节点（§4.4.5）：默认聚合产出 + 终答。本轮在「终态产出」处统一评估
    // （PlanExecute 的聚合产出即其 Final 负载，故 Aggregated 经信 Final 覆盖）。
    enum class ReflectionTrigger : std::uint8_t
    {
        Aggregated,   // 聚合产出
        StepFinished, // 步骤产出
        Final         // ReAct 终答
    };

    struct ReflectionConfig
    {
        std::uint32_t max_reflections{2};
        double pass_score{0.8};
        std::uint32_t no_progress_rounds{2}; // 连续无进展即熔断
        double eps{0.02};                    // 进展判定阈值（Δ<eps 视为无进展）
        std::string criteria;                // 评审标准（可来自 AgentSpec / 请求）
        std::vector<ReflectionTrigger> triggers{ReflectionTrigger::Aggregated,
                                                ReflectionTrigger::Final};
    };

    template <ReasoningMode Inner>
    class ReflectionMode
    {
    public:
        ReflectionMode(Inner inner, IReflector &reflector, ReflectionConfig cfg = {})
            : m_inner(std::move(inner)), m_reflector(reflector), m_cfg(std::move(cfg)) {}

        [[nodiscard]] static constexpr auto kind() noexcept -> ModeKind { return ModeKind::Reflection; }
        [[nodiscard]] static constexpr auto name() noexcept -> std::string_view { return "reflection"; }

        void request_stop() noexcept
        {
            if constexpr (requires { m_inner.request_stop(); })
                m_inner.request_stop();
        }
        [[nodiscard]] bool is_stop_requested() const
        {
            if constexpr (requires { m_inner.is_stop_requested(); })
                return m_inner.is_stop_requested();
            else
                return false;
        }

        auto execute(ModeContext &ctx) -> std::generator<AgentEvent>
        {
            std::string best;
            double best_score = -1.0;
            std::string last_fp;
            std::uint32_t no_progress = 0;
            bool exhausted = false;

            for (std::uint32_t attempt = 0;; ++attempt)
            {
                // 运行内层并复用其事件流（终答负载在此捕获；中间 Final 不外发，由本模式统一收尾）。
                std::string output;
                for (auto &&e : m_inner.execute(ctx))
                {
                    if (e.kind == EventKind::Final)
                    {
                        output = e.payload;
                        continue;
                    }
                    if (e.kind == EventKind::Error)
                    {
                        co_yield std::move(e);
                        co_return;
                    }
                    co_yield std::move(e);
                }

                if (best.empty())
                    best = output;

                if (attempt >= m_cfg.max_reflections)
                    break;

                auto cr = m_reflector.evaluate(output, m_cfg.criteria, ctx);
                if (cr.isError())
                {
                    co_yield AgentEvent{EventKind::Error, {}, cr.error()};
                    co_return;
                }
                const Critique &c = cr.value();
                co_yield AgentEvent{EventKind::ReflectionEvaluated, c.to_json().dump()};

                if (c.passed || c.score >= m_cfg.pass_score)
                {
                    best = output;
                    best_score = c.score;
                    break;
                }

                // §4.4.4 无效循环保护：反馈原地打转 → 立即熔断。
                if (!last_fp.empty() && c.fingerprint == last_fp)
                {
                    exhausted = true;
                    break;
                }
                // score 连续无进展 → 熔断。
                if (c.score <= best_score + m_cfg.eps)
                    ++no_progress;
                else
                {
                    no_progress = 0;
                    best = output;
                    best_score = c.score;
                }
                if (no_progress >= m_cfg.no_progress_rounds)
                {
                    exhausted = true;
                    break;
                }

                last_fp = c.fingerprint;
                append_feedback(ctx, c.feedback.empty() && !c.issues.empty() ? c.issues.front() : c.feedback);
            }

            if (exhausted)
                co_yield AgentEvent{EventKind::ReflectionExhausted, last_fp};
            co_yield AgentEvent{EventKind::Final, best};
        }

    private:
        // 把反馈注入 `ctx.request_body.messages`（内层每轮据此重新生成）。
        static void append_feedback(ModeContext &ctx, const std::string &feedback)
        {
            if (!ctx.request_body.is_object())
            {
                nlohmann::json body = nlohmann::json::object();
                body["messages"] = nlohmann::json::array(
                    {nlohmann::json::object({{"role", "user"},
                                             {"content", ctx.request_body.is_string()
                                                             ? ctx.request_body.get<std::string>()
                                                             : ctx.request_body.dump()}})});
                ctx.request_body = std::move(body);
            }
            if (!ctx.request_body.contains("messages") || !ctx.request_body["messages"].is_array())
                ctx.request_body["messages"] = nlohmann::json::array();
            ctx.request_body["messages"].push_back(
                nlohmann::json::object({{"role", "user"}, {"content", "[评审反馈] " + feedback}}));
        }

        Inner m_inner;
        IReflector &m_reflector;
        ReflectionConfig m_cfg;
    };

} // namespace AE::Engine
