#pragma once

/// @file Mode/IPlanner.hpp
/// @brief 规划器抽象 + 默认 LLM 规划实现（Docs/01 §4.3.2）。
///
/// @details
/// - `IPlanner` 仅两操作：`plan` / `replan`；每次规划一次调用（边界，非热路径）。
/// - `LlmPlanner` 用 `ctx.llm` 生成 JSON 计划，经 `SelfHealingJson` 自愈解析。
/// - `FixedPlanner` 返回固定计划（确定性测试 / 编译探针用）。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <string>
#include <string_view>
#include <utility>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Error.hpp"
#include "Core/Logger.hpp"

#include "AEType.hpp"        // SelfHealingJson
#include "LLMAdapter.hpp"    // LLMAdapter
#include "Mode/ModeContext.hpp"
#include "Mode/Plan.hpp"

namespace AE::Engine
{

    class IPlanner
    {
    public:
        virtual ~IPlanner() = default;
        virtual auto plan(std::string_view goal, ModeContext &ctx) -> Core::Result<Plan> = 0;
        virtual auto replan(const Plan &prev, const ModeContext &ctx, const Deviation &d)
            -> Core::Result<Plan> = 0;
    };

    // ---------------------------------------------------- LLM 规划器
    // 规划质量决定上限（AgentMode.md §一.2）：可经构造参数指定更强模型（model 覆盖）。
    class LlmPlanner final : public IPlanner
    {
    public:
        explicit LlmPlanner(std::string model_override = {}) : m_model(std::move(model_override)) {}

        auto plan(std::string_view goal, ModeContext &ctx) -> Core::Result<Plan> override
        {
            const char *kSys =
                "你是任务规划器。把用户目标拆解为可执行的步骤 DAG，仅输出 JSON："
                "{\"goal\":\"...\",\"steps\":[{\"id\":1,\"description\":\"...\",\"depends_on\":[]}]}。"
                "要求：步骤尽量原子、可独立执行；depends_on 用步骤 id 表达前置依赖；不要输出多余文字。";
            nlohmann::json body = build_body(ctx, kSys, std::string{goal});
            auto text = collect_text(ctx.llm, body);
            if (text.isError())
                return Core::Result<Plan>::Error(text.error());
            return parse_plan(text.value(), /*revision=*/0, std::string{goal});
        }

        auto replan(const Plan &prev, const ModeContext &ctx, const Deviation &d)
            -> Core::Result<Plan> override
        {
            const char *kSys =
                "你是任务重规划器。基于「原计划」与「偏差」，仅修订受影响分支，输出修订后的完整 JSON 计划"
                "（保持相同 JSON 结构，不要输出多余文字）。";
            nlohmann::json user = nlohmann::json::object();
            user["goal"] = prev.goal;
            user["previous_plan"] = prev.to_json();
            user["deviation"] = d.to_json();
            nlohmann::json body = build_body(ctx, kSys, user.dump());
            auto text = collect_text(ctx.llm, body);
            if (text.isError())
                return Core::Result<Plan>::Error(text.error());
            return parse_plan(text.value(), prev.revision + 1, prev.goal);
        }

    private:
        auto build_body(const ModeContext &ctx, std::string_view sys, const std::string &user)
            -> nlohmann::json
        {
            nlohmann::json body = nlohmann::json::object();
            const auto &src = ctx.request_body;
            if (src.is_object())
            {
                for (const char *k : {"model", "temperature", "top_p", "max_tokens"})
                    if (src.contains(k))
                        body[k] = src[k];
            }
            if (!m_model.empty())
                body["model"] = m_model;
            body["stream"] = false;
            body["messages"] = nlohmann::json::array(
                {nlohmann::json::object({{"role", "system"}, {"content", std::string{sys}}}),
                 nlohmann::json::object({{"role", "user"}, {"content", user}})});
            return body;
        }

        static auto collect_text(LLMAdapter &llm, const nlohmann::json &body) -> Core::Result<std::string>
        {
            std::string out;
            try
            {
                for (auto &&d : llm.stream(body))
                    if (d.kind == LlmDelta::Kind::Text)
                        out += d.text_content;
                    else if (d.kind == LlmDelta::Kind::Error)
                        return Core::Result<std::string>::Error(
                            Core::Error(Core::ErrorCode::NetworkError, d.error_message));
            }
            catch (const std::exception &e)
            {
                return Core::Result<std::string>::Error(
                    Core::Error(Core::ErrorCode::NetworkError, std::string{e.what()}));
            }
            if (out.empty())
                return Core::Result<std::string>::Error(
                    Core::Error(Core::ErrorCode::SerializationError, "planner: empty LLM response"));
            return Core::Result<std::string>::Ok(std::move(out));
        }

        static auto parse_plan(const std::string &raw, std::uint32_t revision, const std::string &goal_fallback)
            -> Core::Result<Plan>
        {
            auto [json, level] = SelfHealingJson::repairWithFallback(raw);
            (void)level;
            if (!json.has_value() || !json->is_object())
                return Core::Result<Plan>::Error(
                    Core::Error(Core::ErrorCode::SerializationError, "planner: response is not a JSON object"));
            Plan p = Plan::from_json(*json);
            p.revision = revision;
            if (p.goal.empty())
                p.goal = goal_fallback;
            std::string why;
            if (!p.validate(&why))
                return Core::Result<Plan>::Error(
                    Core::Error(Core::ErrorCode::SerializationError, "planner: invalid plan: " + why));
            return Core::Result<Plan>::Ok(std::move(p));
        }

        std::string m_model; // 空 = 沿用请求默认模型
    };

    // ---------------------------------------------------- 固定计划器（确定性）
    class FixedPlanner final : public IPlanner
    {
    public:
        explicit FixedPlanner(Plan p) : m_plan(std::move(p)) {}
        auto plan(std::string_view /*goal*/, ModeContext & /*ctx*/) -> Core::Result<Plan> override
        {
            return Core::Result<Plan>::Ok(m_plan);
        }
        auto replan(const Plan &prev, const ModeContext &, const Deviation &) -> Core::Result<Plan> override
        {
            Plan p = prev;
            ++p.revision;
            return Core::Result<Plan>::Ok(std::move(p));
        }
        void set_plan(Plan p) { m_plan = std::move(p); }

    private:
        Plan m_plan;
    };

} // namespace AE::Engine
