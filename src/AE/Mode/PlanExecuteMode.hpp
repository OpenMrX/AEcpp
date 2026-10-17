#pragma once

/// @file Mode/PlanExecuteMode.hpp
/// @brief 执行模式：Plan-and-Execute（先规划后执行 + 偏差触发 Replan）（Docs/01 §4.3）。
///
/// @details
/// 状态机（§4.3.3）：
///   PLANNING → EXECUTING → EVALUATING ─┬─▶ STEP_DONE →(还有步) EXECUTING
///                                      ├─▶ REPLAN → PLANNING(rev+1)   [≤ max_replans]
///                                      └─▶ FAILED → ABORT
///   (全步完成) → AGGREGATING → DONE
///
/// - 步骤执行器 `IStepExecutor` 默认委派 `ReactMode`（每步一个子 Agent 循环）。
/// - 依赖：`depends_on` 均为 Done 才置 Running；默认串行（并行留待后续）。
/// - 预算：`max_steps` / `max_replans` / `step_max_iter`。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <generator>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Logger.hpp"

#include "AEType.hpp"
#include "Mode/ModeContext.hpp"
#include "Mode/ModeKind.hpp"
#include "Mode/IReasoningMode.hpp"
#include "Mode/Plan.hpp"
#include "Mode/IPlanner.hpp"
#include "Mode/ReactMode.hpp"

namespace AE::Engine
{

    // ---------------------------------------------------- 步骤执行器
    class IStepExecutor
    {
    public:
        virtual ~IStepExecutor() = default;
        virtual auto execute_step(ModeContext &ctx, const PlanStep &step, std::size_t max_iter)
            -> std::generator<AgentEvent> = 0;
    };

    // 默认：每步委派一个子 ReAct 循环（复用注入依赖，独立子上下文）。
    class ReactStepExecutor final : public IStepExecutor
    {
    public:
        auto execute_step(ModeContext &ctx, const PlanStep &step, std::size_t max_iter)
            -> std::generator<AgentEvent> override
        {
            ModeContext sub{ctx.tools, ctx.ctx_source, ctx.llm, ctx.gate, ctx.state, ctx.telemetry};
            sub.human = ctx.human;
            sub.failures = ctx.failures;
            sub.working_memory = ctx.working_memory;
            sub.long_term_memory = ctx.long_term_memory;
            sub.shared_memory = ctx.shared_memory;

            nlohmann::json body = nlohmann::json::object();
            if (ctx.request_body.is_object())
                for (const char *k : {"model", "temperature", "top_p", "max_tokens"})
                    if (ctx.request_body.contains(k))
                        body[k] = ctx.request_body[k];
            body["messages"] = nlohmann::json::array(
                {nlohmann::json::object({{"role", "user"}, {"content", step.description}})});

            sub.begin_run(ctx.session_id + "#step" + std::to_string(step.id), body,
                          static_cast<std::uint32_t>(max_iter));
            ReactMode m;
            for (auto &&e : m.execute(sub))
                co_yield std::move(e);
        }
    };

    // ---------------------------------------------------- 聚合器
    class IAggregator
    {
    public:
        virtual ~IAggregator() = default;
        virtual auto aggregate(const Plan &plan, ModeContext &ctx) -> Core::Result<std::string> = 0;
    };

    class ConcatAggregator final : public IAggregator
    {
    public:
        auto aggregate(const Plan &plan, ModeContext & /*ctx*/) -> Core::Result<std::string> override
        {
            std::string out;
            for (const auto &s : plan.steps)
                if (!s.result.empty())
                {
                    out += "[step ";
                    out += std::to_string(s.id);
                    out += "] ";
                    out += s.result;
                    out += "\n";
                }
            if (out.empty())
                out = "(no step results)";
            return Core::Result<std::string>::Ok(std::move(out));
        }
    };

    // ---------------------------------------------------- 配置
    struct PlanExecuteConfig
    {
        std::uint32_t max_steps = 16;
        std::uint32_t max_replans = 2;
        std::size_t step_max_iter = 8;
    };

    // ---------------------------------------------------- 模式
    class PlanExecuteMode final
    {
    public:
        explicit PlanExecuteMode(PlanExecuteConfig cfg = {},
                                 std::unique_ptr<IPlanner> planner = nullptr,
                                 std::unique_ptr<IStepExecutor> step = nullptr,
                                 std::unique_ptr<IAggregator> agg = nullptr)
            : m_cfg(cfg),
              m_planner(planner ? std::move(planner) : std::make_unique<LlmPlanner>()),
              m_step(step ? std::move(step) : std::make_unique<ReactStepExecutor>()),
              m_agg(agg ? std::move(agg) : std::make_unique<ConcatAggregator>())
        {
        }

        [[nodiscard]] static constexpr auto kind() noexcept -> ModeKind
        {
            return ModeKind::PlanExecute;
        }
        [[nodiscard]] static constexpr auto name() noexcept -> std::string_view { return "plan_execute"; }

        void request_stop() noexcept { m_stop.request_stop(); }
        [[nodiscard]] bool is_stop_requested() const { return m_stop.stop_requested(); }

        auto execute(ModeContext &ctx) -> std::generator<AgentEvent>
        {
            m_stop = std::stop_source{};
            std::stop_token st = m_stop.get_token();
            const std::string goal = extract_goal(ctx.request_body);

            // ---- PLANNING ----
            Plan plan;
            {
                auto planned = m_planner->plan(goal, ctx);
                if (planned.isError())
                {
                    co_yield AgentEvent{EventKind::Error, {}, planned.error()};
                    co_return;
                }
                plan = std::move(planned.value());
                std::string why;
                if (!plan.validate(&why))
                {
                    co_yield AgentEvent{EventKind::Error, {},
                                        Core::Error{Core::ErrorCode::InvalidState,
                                                    "planner produced invalid plan: " + why}};
                    co_return;
                }
            }
            co_yield AgentEvent{EventKind::PlanCreated, plan.to_json().dump()};

            std::uint32_t replans = 0;
            std::uint32_t executed = 0;

            for (;;)
            {
                if (st.stop_requested())
                {
                    co_yield AgentEvent{EventKind::Error, {}, Core::Error{"Cancelled"}};
                    co_return;
                }

                PlanStep *ready = pick_ready(plan);
                if (!ready)
                {
                    if (all_settled(plan))
                        break; // 全步完成 → 聚合
                    // 有 Pending 但无就绪（依赖环/阻塞）→ PlanInvalid
                    Deviation d;
                    d.kind = Deviation::Kind::PlanInvalid;
                    d.detail = "no executable step (blocked or cyclic)";
                    if (!co_await_replan(plan, ctx, d, replans))
                        co_return;
                    co_yield AgentEvent{EventKind::PlanCreated, plan.to_json().dump()};
                    continue;
                }

                if (executed++ >= m_cfg.max_steps)
                {
                    Deviation d;
                    d.kind = Deviation::Kind::BudgetExceeded;
                    d.detail = "max_steps exceeded";
                    Core::Logger::getInstance().warningFmt("[AE:Plan] {}", d.detail);
                    // 预算超限：不 replan，直接聚合已完成步骤（降级）。
                    break;
                }

                ready->status = PlanStatus::Running;
                co_yield AgentEvent{EventKind::StepStarted, ready->to_json().dump()};

                std::string last_final;
                bool failed = false;
                Core::Error step_err;

                for (auto &&e : m_step->execute_step(ctx, *ready, m_cfg.step_max_iter))
                {
                    if (e.kind == EventKind::Final)
                        last_final = e.payload;
                    else if (e.kind == EventKind::Error)
                    {
                        failed = true;
                        step_err = e.error_info;
                    }
                    co_yield std::move(e);
                }

                if (!failed)
                {
                    ready->status = PlanStatus::Done;
                    ready->result = last_final.empty() ? std::string{"(done)"} : last_final;
                    co_yield AgentEvent{EventKind::StepFinished, ready->to_json().dump()};
                    continue;
                }

                ready->status = PlanStatus::Failed;
                ready->error = AgentError::ToolExec;
                co_yield AgentEvent{EventKind::StepFinished, ready->to_json().dump()};

                Deviation d;
                d.kind = Deviation::Kind::FailedStep;
                d.step_id = ready->id;
                d.detail = step_err.message();
                co_yield AgentEvent{EventKind::ReplanTriggered, d.to_json().dump()};
                if (!co_await_replan(plan, ctx, d, replans))
                {
                    co_yield AgentEvent{EventKind::Error, {}, step_err};
                    co_return;
                }
                co_yield AgentEvent{EventKind::PlanCreated, plan.to_json().dump()};
            }

            // ---- AGGREGATING ----
            co_yield AgentEvent{EventKind::Aggregating};
            auto agg = m_agg->aggregate(plan, ctx);
            if (agg.isError())
            {
                co_yield AgentEvent{EventKind::Error, {}, agg.error()};
                co_return;
            }
            co_yield AgentEvent{EventKind::Final, agg.value()};
        }

    private:
        // 返回首个就绪步骤（Pending 且依赖均 Done/Skipped）。
        static auto pick_ready(Plan &plan) -> PlanStep *
        {
            for (auto &s : plan.steps)
            {
                if (s.status != PlanStatus::Pending)
                    continue;
                bool ok = true;
                for (auto d : s.depends_on)
                {
                    const auto *dep = plan.find(d);
                    if (!dep || (dep->status != PlanStatus::Done && dep->status != PlanStatus::Skipped))
                    {
                        ok = false;
                        break;
                    }
                }
                if (ok)
                    return &s;
            }
            return nullptr;
        }

        static auto all_settled(const Plan &plan) -> bool
        {
            for (const auto &s : plan.steps)
                if (s.status == PlanStatus::Pending || s.status == PlanStatus::Running)
                    return false;
            return true;
        }

        // Replan 并保留已完成步骤（按 id 对齐）；返回 false 表示重规划耗尽/失败。
        auto co_await_replan(Plan &plan, ModeContext &ctx, const Deviation &d, std::uint32_t &replans) -> bool
        {
            if (replans >= m_cfg.max_replans)
            {
                Core::Logger::getInstance().warningFmt(
                    "[AE:Plan] replan budget exhausted ({}), aborting", m_cfg.max_replans);
                return false;
            }
            ++replans;
            auto rp = m_planner->replan(plan, ctx, d);
            if (rp.isError())
            {
                Core::Logger::getInstance().errorFmt("[AE:Plan] replan failed: {}", rp.error().message());
                return false;
            }
            Plan next = std::move(rp.value());
            std::string why;
            if (!next.validate(&why))
            {
                Core::Logger::getInstance().errorFmt("[AE:Plan] replan invalid: {}", why);
                return false;
            }
            // 保留前一计划中已完成的步骤结果（按 id）。
            for (auto &s : next.steps)
            {
                const auto *prev = plan.find(s.id);
                if (prev && prev->status == PlanStatus::Done)
                {
                    s.status = PlanStatus::Done;
                    s.result = prev->result;
                }
            }
            plan = std::move(next);
            return true;
        }

        static auto extract_goal(const nlohmann::json &req) -> std::string
        {
            if (req.is_string())
                return req.get<std::string>();
            if (req.is_object() && req.contains("messages") && req["messages"].is_array())
            {
                for (auto it = req["messages"].rbegin(); it != req["messages"].rend(); ++it)
                {
                    const auto &m = *it;
                    if (m.is_object() && m.value("role", std::string{}) == "user")
                    {
                        if (m.contains("content") && m["content"].is_string())
                            return m["content"].get<std::string>();
                    }
                }
            }
            return req.is_object() ? req.value("goal", std::string{}) : std::string{};
        }

        PlanExecuteConfig m_cfg;
        std::unique_ptr<IPlanner> m_planner;
        std::unique_ptr<IStepExecutor> m_step;
        std::unique_ptr<IAggregator> m_agg;
        std::stop_source m_stop;
    };

    static_assert(ReasoningMode<PlanExecuteMode>, "PlanExecuteMode must satisfy the ReasoningMode contract");

} // namespace AE::Engine
